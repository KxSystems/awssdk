#pragma once

#include <aws/core/Aws.h>
#include <aws/core/utils/logging/LogLevel.h>

#include <string>

// k.h last: its short macros (R, O, Z, P, U, ...) collide with AWS SDK
// identifiers, e.g. `template<class R, class E> class Outcome`.
#include "k.h"

namespace awssdk {

class AwsSession {
  private:
    Aws::SDKOptions options;
    std::string log_prefix;
    bool initialized = false;
    bool atexit_registered = false;

  public:
    ~AwsSession() = default;
    AwsSession(const AwsSession&) = delete;
    AwsSession& operator=(const AwsSession&) = delete;

    static AwsSession& getInstance();
    bool initialize(K options_k);
    bool shutDown();
    bool isInitialized() const;

  private:
    AwsSession() = default;
};

K initialize(K options);
K shutDown(K unused);

}
