#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MVII_NATIVE_BRIDGE_DEVICE "/dev/native0"
#define MVII_NATIVE_BRIDGE_ABI_VERSION 1u

/* Stable ioctl command numbers shared by the tiny Virtua launchers and MVII. */
#define MVII_NATIVE_IOCTL_QUERY  0x4D560100ul
#define MVII_NATIVE_IOCTL_LAUNCH 0x4D560101ul

typedef enum mvii_native_guest_kind {
    MVII_NATIVE_GUEST_GBA_ARM7TDMI = 1,
    MVII_NATIVE_GUEST_VITA_ARMV7 = 2,
    MVII_NATIVE_GUEST_HORIZON_AARCH64 = 3,
} mvii_native_guest_kind;

#define MVII_NATIVE_KIND_BIT(kind) (1u << ((uint32_t)(kind) - 1u))

enum mvii_native_launch_flags {
    MVII_NATIVE_LAUNCH_COOPERATIVE = 1u << 0,
    MVII_NATIVE_LAUNCH_FOREGROUND = 1u << 1,
    MVII_NATIVE_LAUNCH_MAP_PLATFORM_MEMORY = 1u << 2,
    MVII_NATIVE_LAUNCH_FORWARD_INPUT = 1u << 3,
    MVII_NATIVE_LAUNCH_FORWARD_AUDIO = 1u << 4,
    MVII_NATIVE_LAUNCH_FORWARD_VIDEO = 1u << 5,
};

typedef struct mvii_native_query {
    uint32_t abi_version;
    uint32_t supported_guest_kinds;
    uint64_t max_image_size;
    uint64_t capabilities;
} mvii_native_query;

typedef struct mvii_native_launch {
    uint32_t abi_version;
    uint32_t guest_kind;
    int32_t image_fd;
    int32_t auxiliary_fd;
    uint64_t image_size;
    uint64_t auxiliary_size;
    uint64_t guest_entry;
    uint64_t flags;
    int32_t argc;
    uint32_t reserved;
    const char *const *argv;
} mvii_native_launch;

#ifdef __cplusplus
}
#endif
