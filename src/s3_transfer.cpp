// AWS headers first; the project headers below pull in k.h, which must come last.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <aws/core/client/AsyncCallerContext.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/core/utils/threading/Executor.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/AbortMultipartUploadRequest.h>
#include <aws/transfer/TransferHandle.h>
#include <aws/transfer/TransferManager.h>

#include "aws_session.h"
#include "event_channel.h"
#include "handle_registry.h"
#include "retired.h"
#include "s3_client_handle.h"
#include "s3_files.h"
#include "s3_transfer.h"
#include "utils.h"


namespace awssdk {

namespace {

using Aws::Transfer::TransferStatus;

// A transfer manager's SDK objects, in destruction order: the manager holds
// only a raw pointer to the executor, whose destructor joins its threads, so
// the executor is declared first and outlives it.
struct ManagerParts {
    std::shared_ptr<Aws::Utils::Threading::PooledThreadExecutor> executor;
    std::shared_ptr<Aws::Transfer::TransferManager> manager;
    // The manager's own, kept to abort failed uploads with.
    std::shared_ptr<Aws::S3::S3Client> client;
};

struct TransferManagerHandle {
    // Allocated with the handle, so that letting go of it allocates nothing.
    std::shared_ptr<ManagerParts> parts = std::make_shared<ManagerParts>();
    uint64_t part_size = 0;

    // Main thread only, as everything holding one is. Whichever owner lets go
    // last, the parts are retired rather than destroyed: the SDK's tasks hold
    // the manager, and it holds its client. They are idle once no task does.
    ~TransferManagerHandle() {
        const ManagerParts * raw = parts.get();
        retire(std::move(parts), [raw]() {
            return !raw->manager || raw->manager.use_count() == 1;
        });
    }
};

HandleRegistry<TransferManagerHandle> managers("transferManager");

// A download that did not complete still has its file open: the SDK closes it
// only in the handle's destructor, and Windows cannot delete an open file. So
// the handle is retired with the temporary, which goes only once the handle has
// been destroyed, on the main thread, with its file closed.
void retire_unfinished_download(std::shared_ptr<Aws::Transfer::TransferHandle> handle,
        std::shared_ptr<DownloadTarget> target) {
    retire(std::move(handle), nullptr, [target]() mutable { target.reset(); });
}

// Registered as each transfer manager foreign's destructor.
K destroy_transfer_manager(K handle) {
    managers.release(handle);
    return (K)0;
}

// Each thread gets a part buffer, all allocated when the manager is created,
// along with the threads themselves: 64 threads is 320 MB of buffers. Threads
// beyond the client's maxConnections (25 by default) only wait for a
// connection.
constexpr J MAX_THREADS = 64;

// S3 accepts at most this many parts in one multipart upload.
constexpr uint64_t MAX_PARTS = 10000;

// Checked before an upload starts because the manager opens the file itself,
// and would take a directory's bogus size at face value just as putObject
// would. A file needing more parts than S3 allows would otherwise fail only at
// part 10,001, after sending ~50 GB.
void check_upload_source(const TransferManagerHandle & tm, const std::string & filename) {
    const uint64_t size = static_cast<uint64_t>(regular_file_size(filename));
    if (size > tm.part_size * MAX_PARTS) {
        throw std::runtime_error("unable to upload file: " + filename
            + " needs more than " + std::to_string(MAX_PARTS) + " parts of "
            + std::to_string(tm.part_size / (1024 * 1024)) + " MB");
    }
}

// A single PutObject up to bufferSize (5 MB), a parallel multipart upload
// above it. awss3kdb labelled every upload text/plain; a generic binary type is
// used instead.
std::shared_ptr<Aws::Transfer::TransferHandle> start_upload(const TransferManagerHandle & tm,
        const ObjectFileArgs & args,
        const std::shared_ptr<const Aws::Client::AsyncCallerContext> & context) {
    return tm.parts->manager->UploadFile(args.filename.c_str(), args.bucket.c_str(),
        args.key.c_str(), "application/octet-stream",
        Aws::Map<Aws::String, Aws::String>(), context);
}

// Blocks the q thread until the SDK has let go of a transfer's handle, for at
// most a few seconds. A completed download's file can still be open when its
// status says COMPLETED: up to 5 MB it is fetched in one request whose result
// owns the file stream, and that result goes only when the SDK's task returns,
// after the status is set. The task holds the handle until then, so once this
// is its only owner the file is closed -- which a rename needs on Windows.
void wait_for_release(const std::shared_ptr<Aws::Transfer::TransferHandle> & handle) {
    for (int waited = 0; waited < 5000 && handle.use_count() > 1; ++waited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// Blocks the q thread until the transfer finishes. WaitUntilFinished also
// waits for parts still in flight, so nothing of this transfer is running once
// it returns.
bool finished_ok(const std::shared_ptr<Aws::Transfer::TransferHandle> & transfer) {
    transfer->WaitUntilFinished();
    return transfer->GetStatus() == TransferStatus::COMPLETED;
}

// The parts of a failed multipart upload stay in S3, and are billed, until the
// upload is aborted. TransferManager::AbortMultipartUpload cannot do it here:
// it only aborts a canceled transfer, and a failed one can never become
// canceled, so it would do nothing. The upload is aborted directly instead.
// Returns any abort failure, to report alongside the transfer's own error.
//
// The manager sets a transfer's final status only once no part is pending or
// queued, so after that a single abort leaves nothing behind.
std::string abort_failed_upload(const TransferManagerHandle & tm,
        const Aws::Transfer::TransferHandle & transfer) {
    if (!transfer.IsMultipart() || transfer.GetMultiPartId().empty()) { return ""; }

    Aws::S3::Model::AbortMultipartUploadRequest request;
    request.SetBucket(transfer.GetBucketName());
    request.SetKey(transfer.GetKey());
    request.SetUploadId(transfer.GetMultiPartId());
    const auto outcome = tm.parts->client->AbortMultipartUpload(request);
    return outcome.IsSuccess() ? "" : "; abort failed: " + s3_error(outcome.GetError());
}

// --- asynchronous transfers ------------------------------------------------
//
// The SDK reports a transfer's progress on its own threads, which must never
// touch q objects. Each report is copied into plain data and posted to q's
// main thread (event_channel.h), where it is matched to its transfer and the
// q callback is called. Everything below the status callback runs only on the
// main thread, so the registry needs no lock.

// Carried by every async transfer, to tell its reports apart from those of
// blocking transfers on the same manager, and to find it in the registry.
class AsyncContext : public Aws::Client::AsyncCallerContext {
public:
    explicit AsyncContext(uint64_t id_) : id(id_) {}
    const uint64_t id;
};

// One async transfer, from its start until its final report is delivered.
struct AsyncTransfer {
    AsyncTransfer(K callback_, bool upload_) : callback(r1(callback_)), upload(upload_) {}
    // Main thread only, like everything that destroys one.
    ~AsyncTransfer() { r0(callback); }

    AsyncTransfer(const AsyncTransfer &) = delete;
    AsyncTransfer & operator=(const AsyncTransfer &) = delete;

    K callback;
    const bool upload;
    std::string bucket;
    std::string key;
    std::string filename;

    // Held so that dropping the q handle mid-transfer is safe, and so that the
    // last release happens here, on the main thread: on one of the pool's own
    // threads, the executor's destructor would wait for itself.
    std::shared_ptr<TransferManagerHandle> tm;
    std::shared_ptr<Aws::Transfer::TransferHandle> handle;
    // Downloads only.
    std::shared_ptr<DownloadTarget> target;

    // A completed download waiting for the SDK to let go of its handle before
    // the rename (see deliver). The counts its COMPLETED report carried are
    // kept in final_transferred and final_total meanwhile.
    bool awaiting_release = false;

    // Set once the outcome has been acted on -- the temporary renamed or
    // removed, a failed upload aborted -- by the final report or by shutDown.
    bool settled = false;
    TransferStatus final_status = TransferStatus::CANCELED;
    std::string final_error;
    J final_transferred = 0;
    J final_total = 0;
};

// Never destroyed: an entry still here at exit would otherwise be destroyed
// after q, releasing its callback into a runtime that is gone.
std::map<uint64_t, std::unique_ptr<AsyncTransfer>> & in_flight() {
    static auto * transfers = new std::map<uint64_t, std::unique_ptr<AsyncTransfer>>();
    return *transfers;
}
uint64_t next_id = 1;

bool is_final(TransferStatus status) {
    switch (status) {
        case TransferStatus::NOT_STARTED:
        case TransferStatus::IN_PROGRESS:
            return false;
        default:
            return true;
    }
}

// awss3kdb's names, which SP's callback compares against.
const char * status_name(TransferStatus status) {
    switch (status) {
        case TransferStatus::EXACT_OBJECT_ALREADY_EXISTS: return "EXACT_OBJECT_ALREADY_EXISTS";
        case TransferStatus::NOT_STARTED: return "NOT_STARTED";
        case TransferStatus::IN_PROGRESS: return "IN_PROGRESS";
        case TransferStatus::CANCELED: return "CANCELED";
        case TransferStatus::FAILED: return "FAILED";
        case TransferStatus::COMPLETED: return "COMPLETED";
        case TransferStatus::ABORTED: return "ABORTED";
        default: return "UNKNOWN";
    }
}

// Acts on a transfer's outcome, once. A completed download's temporary is
// renamed over the destination before the q callback hears of it, and an
// unsuccessful one's is removed; a failed or canceled multipart upload is
// aborted. Then hands its SDK objects over to be retired: nothing needed to
// report the outcome may hold them, since the callback may call shutDown,
// which has to be able to wait for them.
void settle(AsyncTransfer & t, TransferStatus status, std::string error,
        J transferred, J total) {
    if (t.settled) { return; }
    // What the SDK reported, deliberately, rather than the outcome below: a
    // completed download is settled only once its file is closed -- either the
    // SDK has let go of the handle (deliver), or shutDown has waited for every
    // task to finish -- so even if the rename then fails, the temporary can be
    // removed straight away.
    const bool sdk_completed = status == TransferStatus::COMPLETED;
    if (sdk_completed && t.target) {
        t.handle.reset();
        try {
            t.target->commit();
        } catch (const std::exception & exc) {
            status = TransferStatus::FAILED;
            error = exc.what();
        }
    }
    if (t.upload && t.tm && t.handle
            && (status == TransferStatus::FAILED || status == TransferStatus::CANCELED)) {
        error += abort_failed_upload(*t.tm, *t.handle);
    }
    if (t.target && !sdk_completed) {
        retire_unfinished_download(std::move(t.handle), std::move(t.target));
    }
    t.handle.reset();
    t.target.reset();
    t.tm.reset();

    t.settled = true;
    t.final_status = status;
    t.final_error = error;
    t.final_transferred = transferred;
    t.final_total = total;
}

// Calls the q callback with a report in awss3kdb's shape, so existing callers
// such as SP's download manager work unchanged. An error in the callback is
// reported and goes no further: it must not stop other transfers' reports.
void report(const AsyncTransfer & t, TransferStatus status, J transferred, J total,
        const std::string & error) {
    K keys = ktn(KS, 8);
    kS(keys)[0] = ss((S)"bucket");
    kS(keys)[1] = ss((S)"key");
    kS(keys)[2] = ss((S)"filename");
    kS(keys)[3] = ss((S)"direction");
    kS(keys)[4] = ss((S)"status");
    kS(keys)[5] = ss((S)"transferred");
    kS(keys)[6] = ss((S)"total");
    kS(keys)[7] = ss((S)"error");
    K values = knk(8,
        kpn((S)t.bucket.c_str(), static_cast<J>(t.bucket.size())),
        kpn((S)t.key.c_str(), static_cast<J>(t.key.size())),
        kpn((S)t.filename.c_str(), static_cast<J>(t.filename.size())),
        ks((S)(t.upload ? "UPLOAD" : "DOWNLOAD")),
        ks((S)status_name(status)),
        kj(transferred),
        kj(total),
        kpn((S)error.c_str(), static_cast<J>(error.size())));

    // k consumes its arguments, hence r1 to keep this transfer's reference.
    K result = k(0, (S)"@", r1(t.callback), xD(keys, values), (K)0);
    if (result != nullptr && result->t == -128) {
        std::fprintf(stderr, "awssdk: transfer callback failed: %s\n", result->s);
    }
    if (result != nullptr) { r0(result); }
}

// The continuation of a completed download that waited for the SDK to release
// its handle (see deliver): with the handle gone the file is closed, so the
// rename happens here, at once. It runs on the main thread wherever retired
// objects are pruned -- including shutDown's drain -- so no rename is left
// pending once shutDown returns, to land on a file written after it. Only the
// report is deferred to q's event loop.
void settle_released(uint64_t id) {
    std::map<uint64_t, std::unique_ptr<AsyncTransfer>> & transfers = in_flight();
    auto found = transfers.find(id);
    if (found == transfers.end() || !found->second->awaiting_release) { return; }
    AsyncTransfer & t = *found->second;
    t.awaiting_release = false;
    settle(t, TransferStatus::COMPLETED, "", t.final_transferred, t.final_total);
}

// On the main thread: delivers one report. The first final report for a
// transfer is its last -- the SDK can report a final status more than once,
// when parts finish together -- and anything after it is dropped.
void deliver(uint64_t id, TransferStatus status, J transferred, J total,
        const std::string & error) {
    prune_retired();
    std::map<uint64_t, std::unique_ptr<AsyncTransfer>> & transfers = in_flight();
    auto found = transfers.find(id);
    if (found == transfers.end()) { return; }

    AsyncTransfer & t = *found->second;
    if (t.awaiting_release) {
        // Settled by the release, which then posts the report. A duplicate
        // COMPLETED, or the report shutDown posts, is dropped meanwhile.
        return;
    } else if (!t.settled && !is_final(status)) {
        report(t, status, transferred, total, error);
        return;
    } else if (!t.settled && status == TransferStatus::COMPLETED && t.target
            && !t.target->in_place() && t.handle && t.handle.use_count() > 1) {
        // The SDK may still have the file open: up to 5 MB, the request's
        // result owns the file stream until the SDK's task returns, which is
        // after the status is set. The task holds the handle until then. So
        // the handle is retired, and its release settles the transfer
        // (settle_released) and posts the report, which then comes back here
        // and is delivered from q's event loop, like every other report,
        // rather than from inside retirement. The manager is not needed again.
        t.awaiting_release = true;
        t.final_transferred = transferred;
        t.final_total = total;
        t.tm.reset();
        retire(std::move(t.handle), nullptr, [id]() {
            settle_released(id);
            post_to_main([id]() {
                deliver(id, TransferStatus::COMPLETED, 0, 0, "");
            });
        });
        return;
    }

    // Settled already if shutDown got there first, in which case this report
    // may be an older one queued before it: the outcome and the counts it
    // carries are replaced by the settled ones.
    settle(t, status, error, transferred, total);
    // Out of the registry before the callback runs, which may call back into
    // the module. Destroyed after it, which releases the callback.
    std::unique_ptr<AsyncTransfer> done = std::move(found->second);
    transfers.erase(found);
    report(*done, done->final_status, done->final_transferred, done->final_total,
        done->final_error);
}

// The manager's status callback, on whichever SDK thread reported. Copies the
// report into plain data and posts it; nothing here may touch q.
void on_transfer_status(const Aws::Transfer::TransferManager *,
        const std::shared_ptr<const Aws::Transfer::TransferHandle> & handle) {
    const std::shared_ptr<const AsyncContext> context =
        std::dynamic_pointer_cast<const AsyncContext>(handle->GetContext());
    // A blocking upload or download on the same manager.
    if (!context) { return; }

    const uint64_t id = context->id;
    const TransferStatus status = handle->GetStatus();
    const J transferred = static_cast<J>(handle->GetBytesTransferred());
    const J total = static_cast<J>(handle->GetBytesTotalSize());
    const std::string error = status == TransferStatus::FAILED
        ? s3_error(handle->GetLastError()) : "";
    post_to_main([id, status, transferred, total, error]() {
        deliver(id, status, transferred, total, error);
    });
}

const char * const upload_async_options[] = { "callback" };
const char * const download_async_options[] = { "callback", "inPlace" };

// The required `callback` option: anything q can apply to one argument.
K callback_option(K options, const char * const * allowed, size_t n) {
    const char * unknown = dict_unknown_key(options, allowed, n);
    if (unknown != nullptr) { throw std::invalid_argument(unknown); }

    const J index = dict_find_idx(options, "callback");
    if (index < 0) { throw std::invalid_argument("callback"); }
    // A function can only be held in a general list.
    K values = kK(options)[1];
    if (values->t != 0) { throw std::invalid_argument("callback"); }
    K fn = kK(values)[index];
    // 100-111 are lambdas, primitives, projections, compositions and
    // iterator-derived functions; 101 with g of 0 is the generic null.
    const bool applicable = fn->t >= 100 && fn->t <= 111 && !(fn->t == 101 && fn->g == 0);
    if (!applicable) { throw std::invalid_argument("callback"); }
    return fn;
}

// Registers a transfer before the SDK starts it. Its reports are delivered on
// this thread, so none can arrive before the caller has finished setting it
// up and returned to q.
std::pair<uint64_t, AsyncTransfer *> register_transfer(K callback, bool upload,
        const ObjectFileArgs & args, const std::shared_ptr<TransferManagerHandle> & tm) {
    std::unique_ptr<AsyncTransfer> t(new AsyncTransfer(callback, upload));
    t->bucket = args.bucket;
    t->key = args.key;
    t->filename = args.filename;
    t->tm = tm;
    const uint64_t id = next_id++;
    AsyncTransfer * raw = t.get();
    in_flight()[id] = std::move(t);
    return std::make_pair(id, raw);
}

K generic_null() {
    K none = ka(101);
    none->g = 0;
    return none;
}

}  // namespace

K createTransferManager(K client, K threads, K options) {
    if (const char * error = check_call(options)) { return krr((S)error); }

    try {
        // A long, as awss3kdb took: that is how q writes an integer literal. A
        // pool of no threads could never run a transfer, so it would wait
        // forever.
        if (threads == nullptr || threads->t != -KJ
                || threads->j < 1 || threads->j > MAX_THREADS) {
            throw std::invalid_argument("threads");
        }
        require_no_options(options);
        std::shared_ptr<Aws::S3::S3Client> s3 = get_client(client);

        std::shared_ptr<TransferManagerHandle> handle =
            std::make_shared<TransferManagerHandle>();
        ManagerParts & parts = *handle->parts;
        parts.executor = Aws::MakeShared<Aws::Utils::Threading::PooledThreadExecutor>(
            "awssdk-transfer", static_cast<size_t>(threads->j));

        Aws::Transfer::TransferManagerConfiguration config(parts.executor.get());
        // The manager keeps the client alive, so the q client handle can be
        // dropped while this one is still in use.
        config.s3Client = s3;
        // One part buffer per thread. The SDK allocates this whole budget up
        // front, so its 50 MB default would cost 50 MB even for one thread.
        config.transferBufferMaxHeapSize =
            config.bufferSize * static_cast<uint64_t>(threads->j);
        config.transferStatusUpdatedCallback = on_transfer_status;
        parts.manager = Aws::Transfer::TransferManager::Create(config);
        parts.client = s3;
        handle->part_size = config.bufferSize;

        return managers.wrap(handle, destroy_transfer_manager);
    } catch (const std::exception & exc) {
        return krr(ss((S)exc.what()));
    }
}

K upload(K manager, K bucket, K key, K filename, K options) {
    if (const char * error = check_call(options)) { return krr((S)error); }

    try {
        const ObjectFileArgs args = object_file_args(bucket, key, filename);
        require_no_options(options);
        std::shared_ptr<TransferManagerHandle> tm = managers.get(manager);
        check_upload_source(*tm, args.filename);

        std::shared_ptr<Aws::Transfer::TransferHandle> transfer =
            start_upload(*tm, args, nullptr);
        if (!finished_ok(transfer)) {
            const std::string error = s3_error(transfer->GetLastError());
            throw std::runtime_error(error + abort_failed_upload(*tm, *transfer));
        }
        return kj(static_cast<J>(transfer->GetBytesTotalSize()));
    } catch (const std::exception & exc) {
        return krr_text(exc.what());
    }
}

K download(K manager, K bucket, K key, K filename, K options) {
    if (const char * error = check_call(options)) { return krr((S)error); }

    try {
        const ObjectFileArgs args = object_file_args(bucket, key, filename);
        const bool in_place = in_place_option(options);
        std::shared_ptr<TransferManagerHandle> tm = managers.get(manager);
        std::shared_ptr<DownloadTarget> target =
            std::make_shared<DownloadTarget>(args.filename, in_place);

        std::shared_ptr<Aws::Transfer::TransferHandle> transfer = tm->parts->manager->DownloadFile(
            args.bucket.c_str(), args.key.c_str(), target->path().c_str());
        if (!finished_ok(transfer)) {
            const std::string error = s3_error(transfer->GetLastError());
            retire_unfinished_download(std::move(transfer), target);
            throw std::runtime_error(error);
        }
        // Past the bound the rename is tried anyway, which works wherever an
        // open file can be renamed.
        wait_for_release(transfer);
        const J bytes = static_cast<J>(transfer->GetBytesTotalSize());
        transfer.reset();
        target->commit();
        return kj(bytes);
    } catch (const std::exception & exc) {
        return krr_text(exc.what());
    }
}

K uploadAsync(K manager, K bucket, K key, K filename, K options) {
    if (const char * error = check_call(options)) { return krr((S)error); }

    try {
        const ObjectFileArgs args = object_file_args(bucket, key, filename);
        K callback = callback_option(options, upload_async_options,
            sizeof(upload_async_options) / sizeof(*upload_async_options));
        std::shared_ptr<TransferManagerHandle> tm = managers.get(manager);
        check_upload_source(*tm, args.filename);
        attach_event_channel();

        const std::pair<uint64_t, AsyncTransfer *> started =
            register_transfer(callback, true, args, tm);
        try {
            started.second->handle = start_upload(*tm, args,
                std::make_shared<AsyncContext>(started.first));
        } catch (...) {
            in_flight().erase(started.first);
            throw;
        }
        return generic_null();
    } catch (const std::exception & exc) {
        return krr_text(exc.what());
    }
}

K downloadAsync(K manager, K bucket, K key, K filename, K options) {
    if (const char * error = check_call(options)) { return krr((S)error); }

    try {
        const ObjectFileArgs args = object_file_args(bucket, key, filename);
        K callback = callback_option(options, download_async_options,
            sizeof(download_async_options) / sizeof(*download_async_options));
        bool in_place = false;
        dict_require_type(dict_find_bool(options, "inPlace", in_place), "inPlace");
        std::shared_ptr<TransferManagerHandle> tm = managers.get(manager);
        // Probes the destination now, so an unwritable one is reported here
        // rather than through the callback.
        std::shared_ptr<DownloadTarget> target =
            std::make_shared<DownloadTarget>(args.filename, in_place);
        attach_event_channel();

        const std::pair<uint64_t, AsyncTransfer *> started =
            register_transfer(callback, false, args, tm);
        AsyncTransfer & t = *started.second;
        t.target = std::move(target);
        try {
            t.handle = tm->parts->manager->DownloadFile(args.bucket.c_str(), args.key.c_str(),
                t.target->path().c_str(), Aws::Transfer::DownloadConfiguration(),
                std::make_shared<AsyncContext>(started.first));
        } catch (...) {
            in_flight().erase(started.first);
            throw;
        }
        return generic_null();
    } catch (const std::exception & exc) {
        return krr_text(exc.what());
    }
}

void cancel_all_async_transfers() {
    std::map<uint64_t, std::unique_ptr<AsyncTransfer>> & transfers = in_flight();

    // Including managers that only in-flight transfers still hold, the q
    // handle having been dropped.
    std::vector<std::shared_ptr<TransferManagerHandle>> active;
    for (auto & entry : transfers) {
        const std::shared_ptr<TransferManagerHandle> & tm = entry.second->tm;
        if (tm && std::find(active.begin(), active.end(), tm) == active.end()) {
            active.push_back(tm);
        }
    }
    // CancelAll does not wait, and SDK threads must be done before ShutdownAPI.
    for (auto & tm : active) { tm->parts->manager->CancelAll(); }
    for (auto & tm : active) { tm->parts->manager->WaitUntilAllFinished(); }

    for (auto & entry : transfers) {
        AsyncTransfer & t = *entry.second;
        J transferred = 0;
        J total = 0;
        if (t.handle) {
            transferred = static_cast<J>(t.handle->GetBytesTransferred());
            total = static_cast<J>(t.handle->GetBytesTotalSize());
            TransferStatus status = t.handle->GetStatus();
            if (!is_final(status)) { status = TransferStatus::CANCELED; }
            const std::string error = status == TransferStatus::FAILED
                ? s3_error(t.handle->GetLastError()) : "";
            // Releases its SDK objects, before ShutdownAPI.
            settle(t, status, error, transferred, total);
        }

        // Every transfer still gets its final report, which q receives once
        // its event loop runs again. Any report the SDK posted for it is
        // delivered first, but carries the settled outcome.
        const uint64_t id = entry.first;
        post_to_main([id]() {
            deliver(id, TransferStatus::CANCELED, 0, 0, "");
        });
    }
    // The last references to any manager no q handle holds go here, joining
    // its threads.
}

void destroy_all_transfer_managers() {
    managers.sweep();
}

}
