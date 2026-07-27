#include "horizon_servctl_mvii.h"
#include "minos_user_abi.h"

extern const minos_user_abi *minos_current_user_abi;

enum { VIRTUA_O_RDWR = 2 };

static int service_fd = -1;

static int open_service_device(void)
{
    const minos_user_abi *abi = minos_current_user_abi;
    int current = __atomic_load_n(&service_fd, __ATOMIC_ACQUIRE);
    if (current >= 0) return current;
    if (!abi || !abi->open_fn) return -1;
    const long result = abi->open_fn(MVII_HORIZON_SERVICE_DEVICE, VIRTUA_O_RDWR, 0);
    if (result < 0) return -1;
    int expected = -1;
    if (!__atomic_compare_exchange_n(&service_fd, &expected, (int)result, 0,
                                     __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
        if (abi->close_fn) (void)abi->close_fn((int)result);
        return expected;
    }
    return (int)result;
}

int64_t mvii_horizon_servctl(uint32_t command,
                              uint64_t argument1,
                              uint64_t argument2,
                              uint64_t argument3,
                              uint64_t argument4,
                              uint64_t argument5)
{
    if (command > HZN_SCTL_MEMWATCH_GET_CLEAR) return -22;
    const minos_user_abi *abi = minos_current_user_abi;
    const int fd = open_service_device();
    if (fd < 0 || !abi || !abi->ioctl_fn) return -38;

    mvii_horizon_servctl_request request = {0};
    request.abi_version = MVII_HORIZON_SERVICE_ABI_VERSION;
    request.command = command;
    request.arguments[0] = argument1;
    request.arguments[1] = argument2;
    request.arguments[2] = argument3;
    request.arguments[3] = argument4;
    request.arguments[4] = argument5;
    const long status = abi->ioctl_fn(fd, MVII_HORIZON_IOCTL_SERVCTL, &request);
    return status < 0 ? status : request.result;
}

int64_t mvii_horizon_write_buffer(uint64_t horizon_address,
                                  const void *local_buffer, size_t size)
{
    return mvii_horizon_servctl(HZN_SCTL_WRITE_BUFFER, horizon_address,
        (uint64_t)(uintptr_t)local_buffer, size, 0, 0);
}

int64_t mvii_horizon_read_buffer(uint64_t horizon_address,
                                 void *local_buffer, size_t size)
{
    return mvii_horizon_servctl(HZN_SCTL_READ_BUFFER, horizon_address,
        (uint64_t)(uintptr_t)local_buffer, size, 0, 0);
}

int64_t mvii_horizon_map_memory(uint64_t horizon_address,
                                uint64_t local_address, size_t size)
{
    return mvii_horizon_servctl(HZN_SCTL_MAP_MEMORY, horizon_address,
        local_address, size, 0, 0);
}
