// aws_session.h first: it includes the AWS headers before k.h, whose short
// macros would otherwise break them.
#include "aws_session.h"
#include "aws_credentials.h"
#include "s3_client.h"
#include "s3_objects.h"
#include "k.h"

#ifdef _WIN32
    #define EXP __declspec(dllexport)
#else
  #define EXP __attribute__((visibility("default")))
#endif

extern "C" {
  EXP K kexport() {
    K keys = ktn(KS, 0);
    K values = ktn(0, 0);
    #define ADD_FN(s, a) js(&keys,ss((S)#s)); jk(&values,dl((V*)awssdk::s,a));
    ADD_FN(initialize, 1)
    ADD_FN(shutDown, 1)
    ADD_FN(getCredentials, 1)
    ADD_FN(createClient, 1)
    ADD_FN(listObjects, 3)
    ADD_FN(listObjectsMetadata, 3)
    return xD(keys, values);
  }
}
