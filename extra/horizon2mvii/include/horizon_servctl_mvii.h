#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MVII_HORIZON_SERVICE_DEVICE "/dev/horizon0"
#define MVII_HORIZON_SERVICE_ABI_VERSION 1u
#define MVII_HORIZON_IOCTL_SERVCTL 0x485A0100ul

typedef enum mvii_horizon_servctl_command {
    HZN_SCTL_REGISTER_NAMED_SERVICE = 0,
    HZN_SCTL_GET_CMD,
    HZN_SCTL_PUT_CMD,
    HZN_SCTL_CREATE_SESSION_HANDLE,
    HZN_SCTL_CREATE_COPY_HANDLE,
    HZN_SCTL_GET_PROCESS_ID,
    HZN_SCTL_GET_TITLE_ID,
    HZN_SCTL_WRITE_BUFFER,
    HZN_SCTL_READ_BUFFER,
    HZN_SCTL_MAP_MEMORY,
    HZN_SCTL_WRITE_BUFFER_TO,
    HZN_SCTL_READ_BUFFER_FROM,
    HZN_SCTL_MEMWATCH_GET_CLEAR,
} mvii_horizon_servctl_command;

typedef struct mvii_horizon_servctl_request {
    uint32_t abi_version;
    uint32_t command;
    uint64_t arguments[5];
    int64_t result;
} mvii_horizon_servctl_request;

long mvii_horizon_servctl(unsigned int command,
                          unsigned long argument1,
                          unsigned long argument2,
                          unsigned long argument3,
                          unsigned long argument4,
                          unsigned long argument5);

long mvii_horizon_write_buffer(unsigned long horizon_address,
                               const void *local_buffer, size_t size);
long mvii_horizon_read_buffer(unsigned long horizon_address,
                              void *local_buffer, size_t size);
long mvii_horizon_map_memory(unsigned long horizon_address,
                             unsigned long local_address, size_t size);

#ifdef __cplusplus
}
#endif
