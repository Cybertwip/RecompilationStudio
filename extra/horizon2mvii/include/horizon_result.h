// horizon_result.h — Horizon's Result encoding.
//
// A Result is one 32-bit word: bits 0-8 the module that produced it, bits 9-21
// a module-local description. Zero is success, and every guest checks it that
// way (`R_SUCCEEDED` is `rc == 0`), so nothing here may invent a "soft failure"
// value -- a nonzero Result the guest does not expect is indistinguishable from
// a real one and it will branch on it.
//
// The values below are transcribed from the Horizon kernel module's own table
// in Reference/horizon-linux/include/linux/horizon/result.h, which is where the
// SVC implementations in this front-end were checked against. Only codes this
// front-end actually returns are listed; adding one means being able to say
// which SVC returns it and why.

#ifndef HORIZON_RESULT_H
#define HORIZON_RESULT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t horizon_result;

#define HZN_MODULE_KERNEL 1u
#define HZN_MODULE_HIPC   11u
#define HZN_MODULE_SM     21u

#define HZN_MAKE_RESULT(module, description) \
    ((horizon_result)(((uint32_t)(module) & 0x1FFu) | (((uint32_t)(description) & 0x1FFFu) << 9)))

#define HZN_RESULT_SUCCESS ((horizon_result)0u)

#define HZN_RESULT_OUT_OF_SESSIONS            HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 7)
#define HZN_RESULT_INVALID_ARGUMENT           HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 14)
#define HZN_RESULT_NO_SYNCHRONIZATION_OBJECT  HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 57)
#define HZN_RESULT_TERMINATION_REQUESTED      HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 59)
#define HZN_RESULT_INVALID_SIZE               HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 101)
#define HZN_RESULT_INVALID_ADDRESS            HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 102)
#define HZN_RESULT_OUT_OF_RESOURCE            HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 103)
#define HZN_RESULT_OUT_OF_MEMORY              HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 104)
#define HZN_RESULT_OUT_OF_HANDLES             HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 105)
#define HZN_RESULT_INVALID_CURRENT_MEMORY     HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 106)
#define HZN_RESULT_INVALID_MEMORY_REGION      HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 110)
#define HZN_RESULT_INVALID_PRIORITY           HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 112)
#define HZN_RESULT_INVALID_CORE_ID            HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 113)
#define HZN_RESULT_INVALID_HANDLE             HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 114)
#define HZN_RESULT_INVALID_POINTER            HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 115)
#define HZN_RESULT_INVALID_COMBINATION        HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 116)
#define HZN_RESULT_TIMED_OUT                  HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 117)
#define HZN_RESULT_CANCELLED                  HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 118)
#define HZN_RESULT_OUT_OF_RANGE               HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 119)
#define HZN_RESULT_INVALID_ENUM_VALUE         HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 120)
#define HZN_RESULT_NOT_FOUND                  HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 121)
#define HZN_RESULT_BUSY                       HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 122)
#define HZN_RESULT_SESSION_CLOSED             HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 123)
#define HZN_RESULT_INVALID_STATE              HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 125)
#define HZN_RESULT_PORT_CLOSED                HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 131)
#define HZN_RESULT_LIMIT_REACHED              HZN_MAKE_RESULT(HZN_MODULE_KERNEL, 132)

#ifdef __cplusplus
}
#endif

#endif  // HORIZON_RESULT_H
