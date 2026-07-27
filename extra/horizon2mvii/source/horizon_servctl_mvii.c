#include "horizon_servctl_mvii.h"
#include "minos_user_abi.h"

extern const minos_user_abi *minos_current_user_abi;

enum { VIRTUA_O_RDWR = 2 };

static int service_fd = -1;

static int open_service_device(void)
{
    const minos_user_abi *abi = minos_current_user_abi;
    if (service_fd >= 0) return service_fd;
    if (!abi || !abi->open_fn) return -1;
    const long result = abi->open_fn(MVII_HORIZON_SERVICE_DEVICE, VIRTUA_O_RDWR, 0);
    if (result < 0) return -1;
    service_fd = (int)result;
    return service_fd;
}

long mvii_horizon_servctl(unsigned int command,
                          unsigned long argument1,
                          unsigned long argument2,
                          unsigned long argument3,
                          unsigned long argument4,
                          unsigned long argument5)
{
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
    return status < 0 ? status : (long)request.result;
}

long mvii_horizon_write_buffer(unsigned long horizon_address,
                               const void *local_buffer, size_t size)
{
    return mvii_horizon_servctl(HZN_SCTL_WRITE_BUFFER, horizon_address,
        (unsigned long)(uintptr_t)local_buffer, size, 0, 0);
}

long mvii_horizon_read_buffer(unsigned long horizon_address,
                              void *local_buffer, size_t size)
{
    return mvii_horizon_servctl(HZN_SCTL_READ_BUFFER, horizon_address,
        (unsigned long)(uintptr_t)local_buffer, size, 0, 0);
}

long mvii_horizon_map_memory(unsigned long horizon_address,
                             unsigned long local_address, size_t size)
{
    return mvii_horizon_servctl(HZN_SCTL_MAP_MEMORY, horizon_address,
        local_address, size, 0, 0);
}
