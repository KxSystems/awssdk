// AWS headers first; the project headers below pull in k.h, which must come last.
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

#include <sys/stat.h>

#include <aws/core/utils/UUID.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/s3/S3Errors.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "s3_files.h"
#include "utils.h"


namespace awssdk {

namespace {

const char * const download_options[] = { "inPlace" };

}  // namespace

std::string s3_error(const Aws::Client::AWSError<Aws::S3::S3Errors> & error) {
    std::string name = error.GetExceptionName().c_str();
    const int code = static_cast<int>(error.GetResponseCode());
    // A response with no body has nothing to name the error in -- such as the
    // 404 to the HEAD request the transfer manager sends before a download --
    // so the HTTP status stands in for it.
    if (name.empty() && code >= 100) { name = "HTTP " + std::to_string(code); }

    const std::string message = error.GetMessage().c_str();
    if (name.empty()) { return message.empty() ? "request failed" : message; }
    return message.empty() ? name : name + ": " + message;
}

std::runtime_error file_error(const char * operation, const std::string & filename) {
    return std::runtime_error(std::string("unable to ") + operation
        + " file: " + filename);
}

ObjectFileArgs object_file_args(K bucket, K key, K filename) {
    ObjectFileArgs args;
    if (!k_to_str(bucket, args.bucket)) { throw std::invalid_argument("bucket"); }
    if (!k_to_str(key, args.key)) { throw std::invalid_argument("key"); }
    if (!k_to_str(filename, args.filename)) { throw std::invalid_argument("filename"); }
    return args;
}

void require_no_options(K options) {
    const char * unknown = dict_unknown_key(options, nullptr, 0);
    if (unknown != nullptr) { throw std::invalid_argument(unknown); }
}

bool in_place_option(K options) {
    const char * unknown = dict_unknown_key(options, download_options,
        sizeof(download_options) / sizeof(*download_options));
    if (unknown != nullptr) { throw std::invalid_argument(unknown); }

    bool in_place = false;
    dict_require_type(dict_find_bool(options, "inPlace", in_place), "inPlace");
    return in_place;
}

long long regular_file_size(const std::string & filename) {
    // S_IFMT is used rather than S_ISREG because MSVC does not define the
    // latter, and MSVC needs _stat64 because its struct stat holds a 32-bit
    // size.
#ifdef _WIN32
    struct _stat64 info;
    const int found = _stat64(filename.c_str(), &info);
#else
    struct stat info;
    const int found = stat(filename.c_str(), &info);
#endif
    if (found != 0 || (info.st_mode & S_IFMT) != S_IFREG) {
        throw file_error("read", filename);
    }
    return static_cast<long long>(info.st_size);
}

DownloadTarget::DownloadTarget(const std::string & destination, bool in_place)
    : destination_(destination), path_(destination), in_place_(in_place),
      committed_(false), written_(false) {
    const Aws::String uuid = Aws::Utils::UUID::RandomUUID();
    const std::string temporary = destination_ + ".awssdk-" + uuid.c_str() + ".part";

    // Fail before making the S3 request if the destination directory is not
    // writable. Probed with a sibling temporary in both modes, so that in place
    // the destination itself is not disturbed by the check.
    Aws::FStream probe(temporary.c_str(),
        std::ios_base::out | std::ios_base::binary | std::ios_base::trunc);
    if (!probe.is_open()) { throw file_error("write", destination_); }
    probe.close();
    if (probe.fail()) {
        // A throwing constructor has no destructor call, so clean this path
        // explicitly rather than relying on ~DownloadTarget.
        std::remove(temporary.c_str());
        throw file_error("write", destination_);
    }

    if (in_place_) {
        std::remove(temporary.c_str());
        return;
    }
    // The download opens this path again: a retry needs a fresh stream
    // positioned at byte zero.
    path_ = temporary;
}

DownloadTarget::~DownloadTarget() {
    if (!committed_ && (!in_place_ || written_)) { std::remove(path_.c_str()); }
}

void DownloadTarget::commit() {
    if (in_place_) {
        committed_ = true;
        return;
    }
#ifdef _WIN32
    const bool moved = 0 != MoveFileExA(path_.c_str(), destination_.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    // POSIX rename atomically replaces an existing destination because the
    // temporary lives in the same directory.
    const bool moved = 0 == std::rename(path_.c_str(), destination_.c_str());
#endif
    if (!moved) { throw file_error("write", destination_); }
    committed_ = true;
}

}
