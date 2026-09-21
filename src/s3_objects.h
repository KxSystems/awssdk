#pragma once

// This header deliberately declares no AWS types, so that it can be included
// alongside the other project headers. It pulls in k.h, whose short macros
// (R, O, Z, P, U, ...) collide with AWS SDK identifiers, e.g.
// `template<class R, class E> class Outcome`. Include it *after* any AWS
// header in a translation unit, never before.
#include "k.h"


namespace awssdk {
    K listObjects(K client, K bucket, K options);
    K listObjectsMetadata(K client, K bucket, K options);
}
