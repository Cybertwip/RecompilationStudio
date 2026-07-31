// horizon_main.c — the horizon2mvii front-end.
//
// This replaces source/main.c, which opened /dev/native0 and asked it to run
// the guest through MVII_NATIVE_IOCTL_LAUNCH, and source/horizon_servctl_mvii.c,
// which forwarded Horizon service calls to /dev/horizon0 over an ioctl numbered
// 0x485A0100. None of that exists: there is no /dev/native0 and no
// /dev/horizon0 anywhere in PowerEngine, and no MVII_NATIVE_* or MVII_HORIZON_*
// in its kernel. Both were transports to devices that were never written, so
// the old front-end could only ever have failed at its first open().
//
// What happens instead is the WINE model, which is available here for the same
// reason it is for the Vita: 32-bit Horizon and the J36 are the same
// architecture. The guest's own ARM instructions are loaded into this process
// and executed by the Cortex-A7 directly. Nothing is recompiled, nothing is
// interpreted, there is no CPU emulation in this path. All the work is in the
// layer beneath -- Horizon's kernel and its services, reimplemented on MVII.
//
// The ordering is the whole of the front-end:
//
//   1. install the SVC handler, because a Horizon guest reaches its kernel
//      ONLY through `svc` and one executed before the handler exists is the
//      fatal trap MVII's vector has always been
//   2. bring up the IPC layer, so sm: exists before the guest asks for it
//   3. load and relocate the image, collecting every import that cannot be
//      resolved
//   4. bring up the kernel around the loaded image -- it needs to know where
//      the code landed to answer svcQueryMemory
//   5. refuse if anything is missing, by name
//   6. make the image executable, run its initialisers, and branch

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "horizon_image.h"
#include "horizon_ipc.h"
#include "horizon_kernel.h"
#include "horizon_svc.h"

#include "minos_user_abi.h"
#include "vwine/vwine_log.h"
#include "vwine/vwine_package.h"
#include "vwine/vwine_registry.h"

// Defined by R-Dash and filled in by Dash/armv7/crt.s from the vtable MVII
// passes in r12. Declared, never defined here.
extern const minos_user_abi* minos_current_user_abi;

enum {
    EXIT_OK = 0,
    EXIT_ABI_UNAVAILABLE = 120,
    EXIT_SVC_UNAVAILABLE = 121,
    EXIT_KERNEL_UNAVAILABLE = 122,
    EXIT_NO_IMAGE = 123,
    EXIT_LOAD_FAILED = 124,
    EXIT_MISSING_IMPORTS = 125,
};

// How much address space the guest's heap may ever occupy. Reserved whole at
// startup because Horizon guarantees svcSetHeapSize never moves the base and
// MVII cannot extend a mapping in place -- see horizon_kernel.h. Overridable
// from the command line for a guest that wants more.
#define HORIZON_DEFAULT_HEAP (64u * 1024u * 1024u)

static int read_whole_file(const char* path, void** out_data, size_t* out_size)
{
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        vwine_logf("horizon2mvii: cannot open %s\n", path);
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    const long size = ftell(fp);
    if (size <= 0) {
        vwine_logf("horizon2mvii: %s is empty\n", path);
        fclose(fp);
        return -1;
    }
    // fseek rather than rewind: Virtua's POSIX shim does not declare the
    // latter, and this is the same operation with a return value.
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return -1; }

    void* data = malloc((size_t)size);
    if (!data) {
        vwine_logf("horizon2mvii: cannot allocate %ld bytes for %s\n", size, path);
        fclose(fp);
        return -1;
    }
    if (fread(data, 1, (size_t)size, fp) != (size_t)size) {
        vwine_logf("horizon2mvii: short read on %s\n", path);
        free(data);
        fclose(fp);
        return -1;
    }
    fclose(fp);

    *out_data = data;
    *out_size = (size_t)size;
    return 0;
}

int main(int argc, char** argv)
{
    if (!minos_current_user_abi) {
        // Nothing can be reported through the ABI either, so the exit code is
        // as loud as it gets. It means the crt did not pass the vtable in r12.
        return EXIT_ABI_UNAVAILABLE;
    }

    // The guest image. On the device there is never a command line: MVII starts
    // an application by its `.virtua` and passes that path as argv[0] and
    // nothing else, so reading argv[1] here is what made every dashboard launch
    // print a usage line and exit. The guest is the `executable` Studio stages
    // beside the `.virtua` -- for a Switch package, the ExeFS `main`. An
    // explicit path still wins, for bring-up.
    char image_storage[512];
    char data_root[512];
    if (!vwine_package_resolve_image("horizon2mvii", argc, argv, image_storage,
                                     sizeof(image_storage), data_root,
                                     sizeof(data_root))) {
        vwine_logf("horizon2mvii: usage: horizon2mvii <program.nro | program.nso> "
                   "[heap-size-bytes], or launch a staged package that carries "
                   "its own `executable`\n");
        return EXIT_NO_IMAGE;
    }
    const char* image_path = image_storage;
    if (data_root[0]) vwine_logf("horizon2mvii: title data root %s\n", data_root);

    size_t heap_reserve = HORIZON_DEFAULT_HEAP;
    if (argc >= 3 && argv[2] && argv[2][0]) {
        const long requested = strtol(argv[2], NULL, 0);
        if (requested <= 0) {
            vwine_logf("horizon2mvii: \"%s\" is not a usable heap size\n", argv[2]);
            return EXIT_NO_IMAGE;
        }
        heap_reserve = (size_t)requested;
    }

    // Step 1. Before anything guest-shaped exists, because there is no second
    // chance: the first `svc` with no handler installed is fatal.
    if (!horizon_svc_install()) return EXIT_SVC_UNAVAILABLE;

    // Step 2.
    if (!horizon_ipc_init()) {
        horizon_svc_uninstall();
        return EXIT_KERNEL_UNAVAILABLE;
    }

    vwine_logf("horizon2mvii: loading %s\n", image_path);

    void* blob = NULL;
    size_t blob_size = 0;
    if (read_whole_file(image_path, &blob, &blob_size) != 0) {
        horizon_ipc_shutdown();
        horizon_svc_uninstall();
        return EXIT_LOAD_FAILED;
    }

    // Step 3. A Horizon module is self-contained: it imports nothing, because
    // everything it needs is either an SVC (an `svc` instruction, not a symbol)
    // or an IPC command. So the registry is empty, and that is the correct
    // state rather than an unfinished one. It is still passed, and `missing` is
    // still checked, because a module built against a shared library -- an nnSdk
    // sysmodule, or homebrew linking a .nro library -- would land there instead
    // of relocating to nothing.
    static const vwine_registry registry = { NULL, 0 };

    vwine_missing_set missing;
    vwine_missing_reset(&missing);

    horizon_module module;
    if (horizon_image_load(blob, blob_size, image_path, &registry, &missing,
                           &module) != 0) {
        vwine_logf("horizon2mvii: %s could not be loaded\n", image_path);
        free(blob);
        horizon_ipc_shutdown();
        horizon_svc_uninstall();
        return EXIT_LOAD_FAILED;
    }
    // The container has been decoded into its own mapping; the file bytes are
    // no longer referenced.
    free(blob);

    vwine_logf("horizon2mvii: %s is a %s %s module, mapped at %p, entry %p\n",
               image_path, horizon_container_text(module.container),
               horizon_arch_text(module.arch), module.image.mapping.base,
               (void*)module.image.entry);

    // Step 4. After the load, because the kernel describes the code region to
    // svcQueryMemory and cannot do that before it exists.
    if (!horizon_kernel_init(&module, heap_reserve)) {
        horizon_image_release(&module);
        horizon_ipc_shutdown();
        horizon_svc_uninstall();
        return EXIT_KERNEL_UNAVAILABLE;
    }

    // Step 5. An unresolved import is not survivable, and pretending otherwise
    // is the failure mode this tree exists to avoid: the guest would run until
    // it called the missing function and then fault somewhere with no
    // connection to the cause. Every miss was recorded by name during
    // relocation. The list is also the work queue for whoever extends this
    // front-end next.
    if (missing.total > 0) {
        vwine_missing_report(&missing, image_path);
        vwine_logf("horizon2mvii: refusing to start a guest whose imports this "
                   "build cannot satisfy\n");
        horizon_kernel_shutdown();
        horizon_image_release(&module);
        horizon_ipc_shutdown();
        horizon_svc_uninstall();
        return EXIT_MISSING_IMPORTS;
    }

    // Step 6. The cache maintenance has to happen after the LAST write to the
    // image -- relocation was that write -- and before the first instruction
    // fetch from it. A Cortex-A7's I-cache is not coherent with its D-cache,
    // so skipping this works right up until the address being branched to
    // happens to be stale, and then it executes something else.
    if (vwine_image_finalize(&module.image) != 0) {
        vwine_logf("horizon2mvii: could not make the guest image executable; "
                   "branching into it anyway is the exact bug that check "
                   "prevents\n");
        horizon_kernel_shutdown();
        horizon_image_release(&module);
        horizon_ipc_shutdown();
        horizon_svc_uninstall();
        return EXIT_LOAD_FAILED;
    }
    vwine_image_run_initializers(&module.image);

    vwine_logf("horizon2mvii: entering guest at %p\n", (void*)module.image.entry);

    // The branch. From here the CPU is running the guest's own instructions,
    // in System mode, and every `svc` it executes lands in horizon_svc_handler.
    const horizon_result rc = horizon_kernel_run_main_thread(&module);

    const int exit_code = horizon_kernel_exit_requested()
                              ? horizon_kernel_exit_code()
                              : (rc == HZN_RESULT_SUCCESS ? EXIT_OK : (int)rc);
    vwine_logf("horizon2mvii: guest exited, status %d\n", exit_code);

    vwine_image_run_finalizers(&module.image);
    horizon_kernel_shutdown();
    horizon_image_release(&module);
    horizon_ipc_shutdown();
    horizon_svc_uninstall();
    return exit_code;
}
