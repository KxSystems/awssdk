// AWS headers first; the project headers below pull in k.h, which must come last.
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <aws/core/Region.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/config/AWSConfigFileProfileConfigLoader.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/S3ClientConfiguration.h>

#include "aws_session.h"
#include "s3_client.h"
#include "s3_client_handle.h"
#include "utils.h"


namespace awssdk {

namespace {

const char * const client_options[] = {
    "region", "endpointUrl", "virtualAddressing", "noSignRequest",
    "credentials", "profile", "caFile", "caPath", "verifySsl",
    "connectTimeout", "requestTimeout", "maxConnections"
};

// Each client is held in its own heap slot, and a handle points straight at
// that slot. The slot is freed only by the foreign's own destructor, so its
// address can never be recycled while a handle still names it -- which is why
// pointer identity is safe here. shutDown therefore *releases* each client
// without freeing its slot: freeing them would hand those addresses to the
// next client while stale handles still pointed at them, and dropping such a
// handle would then destroy the wrong client.
//
// The set is what makes an arbitrary 112h safe to reject rather than
// dereference.
using ClientSlot = std::shared_ptr<Aws::S3::S3Client>;
std::set<ClientSlot *> live_clients;

// Guarded because the three paths that touch the registry -- createClient, the
// foreign destructor q runs at garbage collection, and the shutDown sweep --
// are serial only while q calls this module from one thread. Enabling setm(1)
// for the async callbacks lifts that guarantee.
std::mutex live_clients_mutex;

// A real pointer round-tripping through the foreign's payload slot, so this is
// a well-defined pointer-to-pointer cast -- static_cast cannot express it,
// since K is an unrelated object pointer type.
ClientSlot * client_slot(K handle) {
    return reinterpret_cast<ClientSlot *>(kK(handle)[1]);
}

// Registered as the foreign's destructor, so q calls it when the last
// reference goes. This is the only place a slot is freed.
K destroy_client(K client) {
    ClientSlot * slot = client_slot(client);
    bool owned = false;
    {
        std::lock_guard<std::mutex> lock(live_clients_mutex);
        owned = live_clients.erase(slot) != 0;
    }
    // Freed outside the lock: releasing a client runs SDK teardown, which must
    // not happen with the registry held.
    if (owned) { delete slot; }
    return (K)0;
}

// Reports a bad option value using the option's own name as the error text,
// matching how initialize reports a bad `loglevel`.
void require(DictLookup lookup, const char * key) {
    if (lookup == DictLookup::WrongType) { throw std::invalid_argument(key); }
}

// Reads a count or a millisecond duration. `floor` is the lowest accepted
// value: 0 for a timeout that means "no limit", 1 for a count. `ceiling` is
// the largest value the destination field can hold: a q long is 64-bit, so
// without this check the narrowing conversion would wrap silently, and
// maxConnections of 2^32 would land as 0 connections rather than being
// rejected. The timeouts' ceiling is platform-dependent, because the SDK
// stores them in a `long` -- 64-bit here, 32-bit on Windows.
J require_long(K options, const char * key, J fallback, J floor, J ceiling) {
    J value = fallback;
    require(dict_find_long(options, key, value), key);
    if (value < floor || value > ceiling) { throw std::invalid_argument(key); }
    return value;
}

constexpr J LONG_FIELD_MAX = static_cast<J>(std::numeric_limits<long>::max());
constexpr J UNSIGNED_FIELD_MAX = static_cast<J>(std::numeric_limits<unsigned>::max());

}  // namespace

K createClient(K options) {
    // This check and the client construction below are not one atomic step: a
    // shutDown running in between would call Aws::ShutdownAPI while a client
    // is still being built. That is safe only because q calls this module from
    // a single thread, so createClient and shutDown cannot overlap. Step 5
    // enables setm(1) for the async callbacks and removes that guarantee, at
    // which point the session needs a read/write guard held across the whole
    // of any operation that uses the SDK -- not just this one.
    if (!AwsSession::getInstance().isInitialized()) {
        return krr("uninitialized");
    }
    // 101 is the generic null, which is how createClient[::] passes "no options".
    if (options == nullptr || (options->t != XD && options->t != 101)) {
        return krr("type");
    }

    try {
        const char * unknown = dict_unknown_key(options, client_options,
            sizeof(client_options) / sizeof(*client_options));
        if (unknown != nullptr) { throw std::invalid_argument(unknown); }

        Aws::Client::ClientConfiguration config;

        std::string region = Aws::Region::AWS_GLOBAL;
        require(dict_find_str(options, "region", region), "region");
        // Assigned as C strings throughout: the SDK's container aliases are
        // only the std:: ones while USE_AWS_MEMORY_MANAGEMENT is off. With it on,
        // Aws::String is a distinct type and a std::string would not convert.
        config.region = region.c_str();

        std::string endpoint_url;
        const DictLookup endpoint = dict_find_str(options, "endpointUrl", endpoint_url);
        require(endpoint, "endpointUrl");
        if (endpoint == DictLookup::Found) { config.endpointOverride = endpoint_url.c_str(); }

        bool virtual_addressing = true;
        require(dict_find_bool(options, "virtualAddressing", virtual_addressing),
            "virtualAddressing");

        bool no_sign_request = false;
        require(dict_find_bool(options, "noSignRequest", no_sign_request),
            "noSignRequest");

        // TLS and connection settings apply whether or not the request is
        // signed: a public bucket still gets fetched over HTTPS.
        std::string ca_file;
        const DictLookup ca = dict_find_str(options, "caFile", ca_file);
        require(ca, "caFile");
        if (ca == DictLookup::Found) { config.caFile = ca_file.c_str(); }

        std::string ca_path;
        const DictLookup ca_dir = dict_find_str(options, "caPath", ca_path);
        require(ca_dir, "caPath");
        if (ca_dir == DictLookup::Found) { config.caPath = ca_path.c_str(); }

        bool verify_ssl = true;
        require(dict_find_bool(options, "verifySsl", verify_ssl), "verifySsl");
        config.verifySSL = verify_ssl;

        // Defaults are awss3kdb's, not the SDK's (1000ms connect, no request
        // timeout), so a migrating caller sees unchanged behaviour.
        //
        // requestTimeoutMs is not a cap on request duration: under curl it is
        // the low-speed time, aborting only after throughput stays under
        // lowSpeedLimit (1 byte/s) for that long, rounded to whole seconds. So
        // it never cuts off a progressing transfer. The whole-request
        // equivalent, httpRequestTimeoutMs, is left at the SDK's no-limit.
        config.connectTimeoutMs = static_cast<long>(require_long(options,
            "connectTimeout", 5 * 1000, 1, LONG_FIELD_MAX));
        config.requestTimeoutMs = static_cast<long>(require_long(options,
            "requestTimeout", 5 * 1000, 0, LONG_FIELD_MAX));
        config.maxConnections = static_cast<unsigned>(require_long(options,
            "maxConnections", 25, 1, UNSIGNED_FIELD_MAX));

        std::string credentials_file;
        const DictLookup creds = dict_find_str(options, "credentials", credentials_file);
        require(creds, "credentials");

        std::string profile = "default";
        const DictLookup named = dict_find_str(options, "profile", profile);
        require(named, "profile");
        // profile selects within `credentials`. On its own there is nothing for
        // it to select from, and falling through to the default chain would
        // ignore it silently; AWS_PROFILE is how that case is expressed.
        //
        // Not checked under noSignRequest, which discards credential selection
        // altogether -- see the branch below. Values are still type-checked
        // above, so a malformed option is reported either way.
        if (!no_sign_request && named == DictLookup::Found
                && creds != DictLookup::Found) {
            throw std::invalid_argument("profile");
        }

        const Aws::S3::S3ClientConfiguration s3_config(config,
            Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
            virtual_addressing);

        std::shared_ptr<Aws::S3::S3Client> client;
        if (no_sign_request) {
            // Handing the client an empty credentials set is what stops it
            // signing, the equivalent of the AWS CLI's --no-sign-request for
            // public buckets. It takes precedence: `credentials` and `profile`
            // are ignored rather than rejected, matching awss3kdb, which
            // branches here before it ever reads them.
            client = std::make_shared<Aws::S3::S3Client>(
                Aws::Auth::AWSCredentials(), nullptr, s3_config);
        } else if (creds == DictLookup::Found) {
            // An explicit credentials file, as used for a mounted secret. The
            // credentials are read once here, so this client will not pick up
            // later edits to the file. That suits fixed keys; expiring
            // temporary credentials would stop working once they lapse, and
            // would need a reloading provider instead.
            Aws::Config::AWSConfigFileProfileConfigLoader loader(credentials_file.c_str());
            if (!loader.Load()) { throw std::invalid_argument("credentials"); }

            const auto & profiles = loader.GetProfiles();
            auto found = profiles.find(profile.c_str());
            if (found == profiles.end()) {
                // No profile was asked for and there is no [default]. A file
                // holding exactly one profile is still unambiguous, and that is
                // how a mounted secret is usually written, so use it. Anything
                // else is reported rather than guessed at.
                if (named == DictLookup::Found || profiles.size() != 1) {
                    throw std::invalid_argument("profile");
                }
                found = profiles.begin();
            }

            client = std::make_shared<Aws::S3::S3Client>(
                found->second.GetCredentials(), nullptr, s3_config);
        } else {
            // The same default chain as getCredentials.
            client = std::make_shared<Aws::S3::S3Client>(s3_config);
        }

        // The unique_ptr keeps owning the slot until *both* the registry
        // insertion and the foreign have succeeded; release() comes last, and
        // never as an argument to a call that could itself throw.
        std::unique_ptr<ClientSlot> slot(new ClientSlot(client));
        {
            std::lock_guard<std::mutex> lock(live_clients_mutex);
            live_clients.insert(slot.get());
        }

        // knk is variadic, so the destructor is passed through untouched; q
        // reads kK(x)[0] as the destructor and kK(x)[1] as the payload.
        K foreign = knk(2, destroy_client, slot.get());
        if (foreign == nullptr) {
            std::lock_guard<std::mutex> lock(live_clients_mutex);
            live_clients.erase(slot.get());
            throw std::runtime_error("alloc");
        }
        foreign->t = 112;
        slot.release();
        return foreign;
    } catch (const std::exception & exc) {
        return krr(ss((S)exc.what()));
    }
}

std::shared_ptr<Aws::S3::S3Client> get_client(K handle) {
    // n is checked as well as the type: a 112h built elsewhere need not have
    // two slots, and slot 1 must exist before it can be read.
    if (handle == nullptr || handle->t != 112 || handle->n != 2) {
        throw std::invalid_argument("client");
    }

    std::lock_guard<std::mutex> lock(live_clients_mutex);
    ClientSlot * slot = client_slot(handle);
    if (live_clients.find(slot) == live_clients.end()) {
        throw std::invalid_argument("client");
    }
    // Emptied by a shutDown sweep: the handle is still ours, but its client is
    // gone. Copied, so the caller's operation holds the client alive
    // independently of the registry.
    if (!*slot) { throw std::invalid_argument("client"); }
    return *slot;
}

void destroy_all_clients() {
    std::vector<ClientSlot> taken;
    {
        std::lock_guard<std::mutex> lock(live_clients_mutex);
        taken.reserve(live_clients.size());
        // Emptied, not freed: the slots stay allocated so that handles which
        // outlive this sweep keep pointing at memory that is still theirs.
        for (ClientSlot * slot : live_clients) {
            taken.push_back(*slot);
            slot->reset();
        }
    }
    // Released outside the lock, as in destroy_client.
}

}
