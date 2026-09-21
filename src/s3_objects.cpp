// AWS headers first; the project headers below pull in k.h, which must come last.
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <aws/s3/S3Client.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/s3/model/Object.h>
#include <aws/s3/model/ObjectStorageClass.h>

#include "aws_session.h"
#include "s3_client_handle.h"
#include "s3_objects.h"
#include "utils.h"


namespace awssdk {

namespace {

const char * const list_options[] = { "prefix", "startAfter", "marker" };

// An S3 API failure, reported to q with the detail the service gave. Callers
// log this text, so it carries the exception name and message rather than one
// of the short tokens used for bad arguments.
std::string s3_error(const Aws::S3::S3Error & error) {
    return std::string(error.GetExceptionName().c_str()) + ": "
        + error.GetMessage().c_str();
}

// q timestamps count nanoseconds from 2000.01.01, so an epoch-millisecond
// value has to be rebased as well as scaled. A DateTime the service left unset
// reads as the epoch, which is reported as a null timestamp rather than as the
// year 1970.
constexpr J MILLIS_EPOCH_TO_2000 = 946684800000LL;
constexpr J NANOS_PER_MILLI = 1000000LL;

J to_timestamp(const Aws::Utils::DateTime & when) {
    const J millis = (J)when.Millis();
    if (millis <= 0) { return nj; }
    return (millis - MILLIS_EPOCH_TO_2000) * NANOS_PER_MILLI;
}

// S3 normally wraps an ETag in literal double quotes. They are stripped to
// expose the underlying identifier. An ETag is not necessarily an MD5 digest:
// multipart uploads and some encryption modes produce values that cannot be
// compared against a locally computed one.
std::string unquoted(const std::string & etag) {
    if (etag.size() >= 2 && etag.front() == '"' && etag.back() == '"') {
        return etag.substr(1, etag.size() - 2);
    }
    return etag;
}

// Every object under `bucket`, following the continuation token so the caller
// always gets the complete listing. Shared by listObjects and
// listObjectsMetadata, which differ only in what they build from the result.
std::vector<Aws::S3::Model::Object> collect(K client, K bucket, K options,
        bool fetch_owner) {
    std::string bucket_name;
    if (!k_to_str(bucket, bucket_name)) { throw std::invalid_argument("bucket"); }

    const char * unknown = dict_unknown_key(options, list_options,
        sizeof(list_options) / sizeof(*list_options));
    if (unknown != nullptr) { throw std::invalid_argument(unknown); }

    Aws::S3::Model::ListObjectsV2Request request;
    request.SetBucket(bucket_name.c_str());
    if (fetch_owner) { request.SetFetchOwner(true); }

    std::string prefix;
    const DictLookup has_prefix = dict_find_str(options, "prefix", prefix);
    dict_require_type(has_prefix, "prefix");
    if (has_prefix == DictLookup::Found) { request.SetPrefix(prefix.c_str()); }

    // `marker` is the awss3kdb spelling, kept as an alias so callers port
    // unchanged; both set StartAfter, whose exclusive-start-key semantics match
    // v1's Marker. Accepting both at once would need a precedence rule nobody
    // asked for, so it is an error.
    std::string start_after;
    const DictLookup has_start = dict_find_str(options, "startAfter", start_after);
    dict_require_type(has_start, "startAfter");
    std::string marker;
    const DictLookup has_marker = dict_find_str(options, "marker", marker);
    dict_require_type(has_marker, "marker");

    if (has_start == DictLookup::Found && has_marker == DictLookup::Found) {
        throw std::invalid_argument("marker");
    }
    if (has_start == DictLookup::Found) {
        request.SetStartAfter(start_after.c_str());
    } else if (has_marker == DictLookup::Found) {
        request.SetStartAfter(marker.c_str());
    }

    // Held by value so the client survives the whole paginated operation. This
    // keeps the object alive but does not make a concurrent Aws::ShutdownAPI
    // safe, since SDK calls must fall between InitAPI and ShutdownAPI: session
    // shutdown must wait until all active SDK operations have finished. Today
    // nothing enforces that beyond q calling this module from one thread.
    std::shared_ptr<Aws::S3::S3Client> s3 = get_client(client);

    // MaxKeys defaults to 1000, so anything larger takes more than one request.
    std::vector<Aws::S3::Model::Object> found;
    for (;;) {
        auto outcome = s3->ListObjectsV2(request);
        if (!outcome.IsSuccess()) { throw std::runtime_error(s3_error(outcome.GetError())); }

        const auto & result = outcome.GetResult();
        const auto & contents = result.GetContents();
        found.insert(found.end(), contents.begin(), contents.end());

        if (!result.GetIsTruncated()) { break; }

        // S3 always supplies the token when the result is truncated. Some
        // S3-compatible servers do not, and re-sending the request without one
        // would fetch the same page forever, so this is a hang rather than a
        // wrong answer if left unchecked.
        const Aws::String & token = result.GetNextContinuationToken();
        if (token.empty()) {
            throw std::runtime_error(
                "truncated listing has no continuation token");
        }
        request.SetContinuationToken(token);
    }
    return found;
}

// The object keys, as the key half of a keyed table.
//
// Named objectKey rather than key: `key` is a reserved word in q, so a column
// of that name cannot be written in a table literal at all, and
// `where key = ...` silently resolves to the key function and matches nothing
// instead of erroring. Neither is worth inheriting from awss3kdb's shape.
K key_table(const std::vector<Aws::S3::Model::Object> & objects) {
    K keys = ktn(0, (J)objects.size());
    for (J i = 0; i < (J)objects.size(); ++i) {
        kK(keys)[i] = kp((S)objects[(size_t)i].GetKey().c_str());
    }
    K column = ktn(KS, 1);
    kS(column)[0] = ss((S)"objectKey");
    return xT(xD(column, knk(1, keys)));
}

}  // namespace

K listObjects(K client, K bucket, K options) {
    if (!AwsSession::getInstance().isInitialized()) {
        return krr("uninitialized");
    }
    // 101 is the generic null, which is how a caller passes "no options".
    if (options == nullptr || (options->t != XD && options->t != 101)) {
        return krr("type");
    }

    try {
        const std::vector<Aws::S3::Model::Object> objects =
            collect(client, bucket, options, false);

        // A keyed table of key!size. Both stream-processor consumers depend on
        // this exact shape: fs/s3.q renames the `key` column and joins on it,
        // and parquet_v1.q indexes it as listing[object]`size. An empty listing
        // still has to come back as a well-formed 0-row keyed table.
        K sizes = ktn(KJ, (J)objects.size());
        for (J i = 0; i < (J)objects.size(); ++i) {
            kJ(sizes)[i] = (J)objects[(size_t)i].GetSize();
        }
        K column = ktn(KS, 1);
        kS(column)[0] = ss((S)"size");

        return xD(key_table(objects), xT(xD(column, knk(1, sizes))));
    } catch (const std::exception & exc) {
        return krr_text(exc.what());
    }
}

K listObjectsMetadata(K client, K bucket, K options) {
    if (!AwsSession::getInstance().isInitialized()) {
        return krr("uninitialized");
    }
    if (options == nullptr || (options->t != XD && options->t != 101)) {
        return krr("type");
    }

    try {
        // Owner is requested here but not by listObjects. It needs no second
        // request -- only a larger response on the same one. Only the canonical
        // ID is exposed: the display name AWS S3 also used to return is now
        // generally empty, so a column for it would carry nothing.
        const std::vector<Aws::S3::Model::Object> objects =
            collect(client, bucket, options, true);
        const J count = (J)objects.size();

        // A superset of listObjects' shape, keyed the same way, so
        // listing[key]`size keeps working against either function.
        K sizes = ktn(KJ, count);
        K modified = ktn(KP, count);
        K etags = ktn(0, count);
        K classes = ktn(KS, count);
        K owner_ids = ktn(0, count);

        for (J i = 0; i < count; ++i) {
            const Aws::S3::Model::Object & object = objects[(size_t)i];
            kJ(sizes)[i] = (J)object.GetSize();
            kJ(modified)[i] = to_timestamp(object.GetLastModified());
            const std::string etag = unquoted(object.GetETag().c_str());
            kK(etags)[i] = kp((S)etag.c_str());
            kS(classes)[i] = ss((S)Aws::S3::Model::ObjectStorageClassMapper::GetNameForObjectStorageClass(
                object.GetStorageClass()).c_str());
            kK(owner_ids)[i] = kp((S)object.GetOwner().GetID().c_str());
        }

        K columns = ktn(KS, 5);
        kS(columns)[0] = ss((S)"size");
        kS(columns)[1] = ss((S)"lastModified");
        kS(columns)[2] = ss((S)"eTag");
        kS(columns)[3] = ss((S)"storageClass");
        kS(columns)[4] = ss((S)"ownerId");
        K values = knk(5, sizes, modified, etags, classes, owner_ids);

        return xD(key_table(objects), xT(xD(columns, values)));
    } catch (const std::exception & exc) {
        return krr_text(exc.what());
    }
}

}
