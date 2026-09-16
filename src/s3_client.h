#pragma once

// This header deliberately declares no AWS types, so that it can be included
// alongside the other project headers. It pulls in k.h, whose short macros
// (R, O, Z, P, U, ...) collide with AWS SDK identifiers, e.g.
// `template<class R, class E> class Outcome`. Include it *after* any AWS
// header in a translation unit, never before.
#include "k.h"


namespace awssdk {
    // Destroys every live S3 client. The clients hold SDK resources, so this
    // has to run before Aws::ShutdownAPI.
    void destroy_all_clients();

    K createClient(K options);
}
