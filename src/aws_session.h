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

// The checks a module function taking an options dictionary starts with: the
// SDK must be initialized, and the options must be a dictionary or the generic
// null (101h), which is how a caller passes "no options". Also destroys any
// retired SDK objects that have become idle. NULL if the call may go ahead,
// otherwise the error to raise -- as `return krr((S)error)`, since krr itself
// returns NULL rather than an error object.
const char * check_call(K options);

}
