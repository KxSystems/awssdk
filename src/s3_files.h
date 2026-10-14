#pragma once

// AWS headers before k.h: k.h's short macros (R, O, Z, P, U, ...) collide with
// AWS SDK identifiers, e.g. `template<class R, class E> class Outcome`. A
// translation unit including this header must not have reached k.h already.
//
// Helpers shared by the object functions (getObject, putObject) and the
// transfer-manager functions (upload, download), which take the same bucket,
// key and local file arguments.
#include <aws/s3/S3Errors.h>

#include <stdexcept>
#include <string>

#include "k.h"


namespace awssdk {

// An S3 API failure, reported to q with the detail the service gave. Callers
// log this text, so it carries the exception name and message rather than one
// of the short tokens used for bad arguments. Takes the base error type, which
// is what a transfer handle reports; an S3Error converts to it.
std::string s3_error(const Aws::Client::AWSError<Aws::S3::S3Errors> & error);

std::runtime_error file_error(const char * operation, const std::string & filename);

struct ObjectFileArgs {
    std::string bucket;
    std::string key;
    std::string filename;
};

// Reads the bucket, key and filename arguments, throwing
// std::invalid_argument with the argument's name if one is not a string.
ObjectFileArgs object_file_args(K bucket, K key, K filename);

// Throws std::invalid_argument with the first key of `options`, if it has any.
// For functions with no options yet: the argument is kept for compatibility
// with awss3kdb and room for additions, so :: and an empty dictionary pass.
void require_no_options(K options);

// Reads the `inPlace` option of a download, rejecting any other key. False,
// the default, means the download goes through a DownloadTarget's temporary.
bool in_place_option(K options);

// The size of `filename`, which must be a regular file, or file_error("read").
// On Linux an fstream opens a directory without error and reports its length
// as INT64_MAX, which would otherwise be sent as a content length.
long long regular_file_size(const std::string & filename);

// Where a download writes, and how it lands at its destination.
//
// By default the object is streamed to a sibling temporary file, which replaces
// the destination only on commit(). That leaves an existing destination
// untouched when S3 or the local write fails, and the rename is atomic on the
// supported platforms because source and destination share a directory.
//
// In place, the object is written straight into the destination, as awss3kdb
// did. A caller that tracks a download by its final path -- its size while in
// flight, or deleting it after a crash -- needs this, at the cost of a failed
// download leaving the destination truncated or partial.
class DownloadTarget {
public:
    DownloadTarget(const std::string & destination, bool in_place);
    ~DownloadTarget();

    DownloadTarget(const DownloadTarget &) = delete;
    DownloadTarget & operator=(const DownloadTarget &) = delete;

    // The file to write the object to: the destination itself when in place,
    // otherwise the temporary.
    const std::string & path() const { return path_; }

    // Moves a completed temporary over the destination. Nothing to do in place.
    void commit();

    // In place, records that the download has begun writing the destination,
    // so that it is removed if the download is not committed: by then it has
    // been truncated, and may hold a partial body or an error document. The
    // removal happens in the destructor, so a stream the SDK still holds open
    // is closed first if the target outlives it -- as it must on Windows,
    // where an open file cannot be deleted.
    void mark_written() { written_ = true; }

private:
    std::string destination_;
    std::string path_;
    bool in_place_;
    bool committed_;
    bool written_;
};

}
