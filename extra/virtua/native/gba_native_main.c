#include <stddef.h>
#include <stdint.h>

#include "minos_user_abi.h"
#include "native_bridge.h"

const minos_user_abi *minos_current_user_abi;

enum {
    VIRTUA_O_RDONLY = 0,
    VIRTUA_O_RDWR = 2,
    EXIT_ABI_UNAVAILABLE = 120,
    EXIT_ROM_OPEN_FAILED = 121,
    EXIT_BRIDGE_OPEN_FAILED = 122,
    EXIT_BRIDGE_QUERY_FAILED = 123,
    EXIT_GBA_UNSUPPORTED = 124,
    EXIT_NATIVE_LAUNCH_FAILED = 125,
};

static size_t text_length(const char *text)
{
    size_t length = 0;
    if (!text)
        return 0;
    while (text[length] != '\0')
        ++length;
    return length;
}

static void write_error(const minos_user_abi *abi, const char *text)
{
    if (abi && abi->write_fn && text)
        (void)abi->write_fn(2, text, text_length(text));
}

static int close_with_result(const minos_user_abi *abi, int bridge_fd, int image_fd, int result)
{
    if (abi && abi->close_fn) {
        if (bridge_fd >= 0)
            (void)abi->close_fn(bridge_fd);
        if (image_fd >= 0)
            (void)abi->close_fn(image_fd);
    }
    return result;
}

int main(int argc, char **argv)
{
    const minos_user_abi *abi = minos_current_user_abi;
    if (!abi || !abi->open_fn || !abi->close_fn || !abi->ioctl_fn) {
        write_error(abi, "GBA Native: Virtua syscall ABI is unavailable.\n");
        return EXIT_ABI_UNAVAILABLE;
    }

    const char *rom_path = "game.gba";
    if (argc > 1 && argv && argv[1] && argv[1][0] != '\0')
        rom_path = argv[1];

    const long image_fd = abi->open_fn(rom_path, VIRTUA_O_RDONLY, 0);
    if (image_fd < 0) {
        write_error(abi, "GBA Native: the packaged ROM could not be opened.\n");
        return EXIT_ROM_OPEN_FAILED;
    }

    const long bridge_fd = abi->open_fn(MVII_NATIVE_BRIDGE_DEVICE, VIRTUA_O_RDWR, 0);
    if (bridge_fd < 0) {
        write_error(abi, "GBA Native: MVII native bridge device is unavailable.\n");
        return close_with_result(abi, -1, (int)image_fd, EXIT_BRIDGE_OPEN_FAILED);
    }

    mvii_native_query query = {0};
    query.abi_version = MVII_NATIVE_BRIDGE_ABI_VERSION;
    if (abi->ioctl_fn((int)bridge_fd, MVII_NATIVE_IOCTL_QUERY, &query) < 0 ||
        query.abi_version != MVII_NATIVE_BRIDGE_ABI_VERSION) {
        write_error(abi, "GBA Native: MVII native bridge ABI negotiation failed.\n");
        return close_with_result(abi, (int)bridge_fd, (int)image_fd,
                                 EXIT_BRIDGE_QUERY_FAILED);
    }
    if ((query.supported_guest_kinds &
         MVII_NATIVE_KIND_BIT(MVII_NATIVE_GUEST_GBA_ARM7TDMI)) == 0) {
        write_error(abi, "GBA Native: this MVII build does not expose native GBA execution.\n");
        return close_with_result(abi, (int)bridge_fd, (int)image_fd,
                                 EXIT_GBA_UNSUPPORTED);
    }

    const char *guest_argv[] = {rom_path, 0};
    mvii_native_launch launch = {0};
    launch.abi_version = MVII_NATIVE_BRIDGE_ABI_VERSION;
    launch.guest_kind = MVII_NATIVE_GUEST_GBA_ARM7TDMI;
    launch.image_fd = (int32_t)image_fd;
    launch.auxiliary_fd = -1;
    launch.guest_entry = 0x08000000u;
    launch.flags = MVII_NATIVE_LAUNCH_COOPERATIVE |
                   MVII_NATIVE_LAUNCH_FOREGROUND |
                   MVII_NATIVE_LAUNCH_MAP_PLATFORM_MEMORY |
                   MVII_NATIVE_LAUNCH_FORWARD_INPUT |
                   MVII_NATIVE_LAUNCH_FORWARD_AUDIO |
                   MVII_NATIVE_LAUNCH_FORWARD_VIDEO;
    launch.argc = 1;
    launch.argv = guest_argv;

    const long result = abi->ioctl_fn((int)bridge_fd, MVII_NATIVE_IOCTL_LAUNCH, &launch);
    if (result < 0) {
        write_error(abi, "GBA Native: MVII rejected the native launch request.\n");
        return close_with_result(abi, (int)bridge_fd, (int)image_fd,
                                 EXIT_NATIVE_LAUNCH_FAILED);
    }
    return close_with_result(abi, (int)bridge_fd, (int)image_fd, (int)result);
}

__attribute__((noreturn)) void sys_exit(int code)
{
    const minos_user_abi *abi = minos_current_user_abi;
    if (abi && abi->exit_fn)
        abi->exit_fn(code);
    for (;;)
        __asm__ volatile("yield" ::: "memory");
}
