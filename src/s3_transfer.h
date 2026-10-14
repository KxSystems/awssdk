#pragma once

// This header deliberately declares no AWS types, so that it can be included
// alongside the other project headers. It pulls in k.h, whose short macros
// (R, O, Z, P, U, ...) collide with AWS SDK identifiers, e.g.
// `template<class R, class E> class Outcome`. Include it *after* any AWS
// header in a translation unit, never before.
#include "k.h"


namespace awssdk {
    // Destroys every live transfer manager. Each one holds a client and a
    // thread pool, so this has to run before destroy_all_clients, and both
    // before Aws::ShutdownAPI.
    void destroy_all_transfer_managers();

    // Cancels every async transfer still running and waits for the SDK's
    // threads to finish with them, settles each one's outcome, and releases
    // their SDK objects; each transfer's final report reaches its q callback
    // once q's event loop runs again. Has to run before
    // destroy_all_transfer_managers.
    void cancel_all_async_transfers();

    K createTransferManager(K client, K threads, K options);
    K upload(K manager, K bucket, K key, K filename, K options);
    K download(K manager, K bucket, K key, K filename, K options);
    K uploadAsync(K manager, K bucket, K key, K filename, K options);
    K downloadAsync(K manager, K bucket, K key, K filename, K options);
}
