#pragma once

// AWS headers before k.h: k.h's short macros (R, O, Z, P, U, ...) collide with
// AWS SDK identifiers, e.g. `template<class R, class E> class Outcome`. A
// translation unit including this header must not have reached k.h already.
//
// This is the AWS-typed companion to s3_client.h, which stays AWS-free so that
// the q entry points can be declared alongside the other project headers.
#include <aws/s3/S3Client.h>

#include <memory>

#include "k.h"


namespace awssdk {
    // The client named by a createClient handle. Resolve handles only through
    // this: it validates them against the registry in s3_client.cpp, see
    // HandleRegistry::get.
    //
    // Throws std::invalid_argument("client") for anything that is not a live
    // handle: a wrong type, a foreign from somewhere else, or one that a
    // shutDown has invalidated.
    std::shared_ptr<Aws::S3::S3Client> get_client(K handle);
}
