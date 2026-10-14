// AWS headers first; the project headers below pull in k.h, which must come last.
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/core/utils/threading/Executor.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/AbortMultipartUploadRequest.h>
#include <aws/transfer/TransferHandle.h>
#include <aws/transfer/TransferManager.h>

#include "aws_session.h"
#include "handle_registry.h"
#include "s3_client_handle.h"
#include "s3_files.h"
#include "s3_transfer.h"
#include "utils.h"


namespace awssdk {

namespace {

struct TransferManagerHandle {
    // Declared first so that it is destroyed last: the manager holds only a
    // raw pointer to the executor, whose destructor joins its threads.
    std::shared_ptr<Aws::Utils::Threading::PooledThreadExecutor> executor;
    std::shared_ptr<Aws::Transfer::TransferManager> manager;
    // The manager's own, kept to abort failed uploads with.
    std::shared_ptr<Aws::S3::S3Client> client;
    uint64_t part_size;
};

HandleRegistry<TransferManagerHandle> managers("transferManager");

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

// Blocks the q thread until the transfer finishes. WaitUntilFinished also
// waits for parts still in flight, so nothing of this transfer is running once
// it returns.
bool finished_ok(const std::shared_ptr<Aws::Transfer::TransferHandle> & transfer) {
    transfer->WaitUntilFinished();
    return transfer->GetStatus() == Aws::Transfer::TransferStatus::COMPLETED;
}

// The parts of a failed multipart upload stay in S3, and are billed, until the
// upload is aborted. TransferManager::AbortMultipartUpload cannot do it here:
// it only aborts a canceled transfer, and a failed one can never become
// canceled, so it would do nothing. The upload is aborted directly instead.
// Returns any abort failure, to report alongside the transfer's own error.
std::string abort_failed_upload(const TransferManagerHandle & tm,
        const Aws::Transfer::TransferHandle & transfer) {
    if (!transfer.IsMultipart() || transfer.GetMultiPartId().empty()) { return ""; }

    Aws::S3::Model::AbortMultipartUploadRequest request;
    request.SetBucket(transfer.GetBucketName());
    request.SetKey(transfer.GetKey());
    request.SetUploadId(transfer.GetMultiPartId());
    const auto outcome = tm.client->AbortMultipartUpload(request);
    return outcome.IsSuccess() ? "" : "; abort failed: " + s3_error(outcome.GetError());
}

}  // namespace

K createTransferManager(K client, K threads, K options) {
    if (!AwsSession::getInstance().isInitialized()) {
        return krr("uninitialized");
    }
    if (options == nullptr || (options->t != XD && options->t != 101)) {
        return krr("type");
    }

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
        handle->executor = Aws::MakeShared<Aws::Utils::Threading::PooledThreadExecutor>(
            "awssdk-transfer", static_cast<size_t>(threads->j));

        Aws::Transfer::TransferManagerConfiguration config(handle->executor.get());
        // The manager keeps the client alive, so the q client handle can be
        // dropped while this one is still in use.
        config.s3Client = s3;
        // One part buffer per thread. The SDK allocates this whole budget up
        // front, so its 50 MB default would cost 50 MB even for one thread.
        config.transferBufferMaxHeapSize =
            config.bufferSize * static_cast<uint64_t>(threads->j);
        handle->manager = Aws::Transfer::TransferManager::Create(config);
        handle->client = s3;
        handle->part_size = config.bufferSize;

        return managers.wrap(handle, destroy_transfer_manager);
    } catch (const std::exception & exc) {
        return krr(ss((S)exc.what()));
    }
}

K upload(K manager, K bucket, K key, K filename, K options) {
    if (!AwsSession::getInstance().isInitialized()) {
        return krr("uninitialized");
    }
    if (options == nullptr || (options->t != XD && options->t != 101)) {
        return krr("type");
    }

    try {
        const ObjectFileArgs args = object_file_args(bucket, key, filename);
        require_no_options(options);
        std::shared_ptr<TransferManagerHandle> tm = managers.get(manager);
        // Checked here because the manager opens the file itself, and would
        // take a directory's bogus size at face value just as putObject would.
        const uint64_t size = static_cast<uint64_t>(regular_file_size(args.filename));
        // Otherwise it would fail only at part 10,001, after sending ~50 GB.
        if (size > tm->part_size * MAX_PARTS) {
            throw std::runtime_error("unable to upload file: " + args.filename
                + " needs more than " + std::to_string(MAX_PARTS) + " parts of "
                + std::to_string(tm->part_size / (1024 * 1024)) + " MB");
        }

        // A single PutObject up to bufferSize (5 MB), a parallel multipart
        // upload above it. awss3kdb labelled every upload text/plain; a generic
        // binary type is used instead.
        std::shared_ptr<Aws::Transfer::TransferHandle> transfer = tm->manager->UploadFile(
            args.filename.c_str(), args.bucket.c_str(), args.key.c_str(),
            "application/octet-stream", Aws::Map<Aws::String, Aws::String>());
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
    if (!AwsSession::getInstance().isInitialized()) {
        return krr("uninitialized");
    }
    if (options == nullptr || (options->t != XD && options->t != 101)) {
        return krr("type");
    }

    try {
        const ObjectFileArgs args = object_file_args(bucket, key, filename);
        const bool in_place = in_place_option(options);
        std::shared_ptr<TransferManagerHandle> tm = managers.get(manager);
        DownloadTarget target(args.filename, in_place);

        std::shared_ptr<Aws::Transfer::TransferHandle> transfer = tm->manager->DownloadFile(
            args.bucket.c_str(), args.key.c_str(), target.path().c_str());
        if (!finished_ok(transfer)) {
            throw std::runtime_error(s3_error(transfer->GetLastError()));
        }
        target.commit();
        return kj(static_cast<J>(transfer->GetBytesTotalSize()));
    } catch (const std::exception & exc) {
        return krr_text(exc.what());
    }
}

void destroy_all_transfer_managers() {
    managers.sweep();
}

}
