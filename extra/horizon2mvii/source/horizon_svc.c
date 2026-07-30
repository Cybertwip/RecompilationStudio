#include "horizon_svc.h"

#include <stdint.h>
#include <string.h>

#include "horizon_ipc.h"
#include "horizon_kernel.h"
#include "minos_user_abi.h"
#include "vwine/vwine_log.h"

extern const minos_user_abi* minos_current_user_abi;

// ── SVC numbers ────────────────────────────────────────────────────────────
//
// Transcribed from
// Reference/horizon-linux/arch/arm64/include/asm/horizon/unistd.h. Gaps in the
// sequence are real: Horizon's table is sparse and the missing numbers are
// services that kernel does not implement either.

#define SVC_SET_HEAP_SIZE                 0x01
#define SVC_SET_MEMORY_ATTRIBUTE          0x03
#define SVC_MAP_MEMORY                    0x04
#define SVC_UNMAP_MEMORY                  0x05
#define SVC_QUERY_MEMORY                  0x06
#define SVC_EXIT_PROCESS                  0x07
#define SVC_CREATE_THREAD                 0x08
#define SVC_START_THREAD                  0x09
#define SVC_EXIT_THREAD                   0x0A
#define SVC_SLEEP_THREAD                  0x0B
#define SVC_GET_THREAD_PRIORITY           0x0C
#define SVC_SET_THREAD_PRIORITY           0x0D
#define SVC_SET_THREAD_CORE_MASK          0x0F
#define SVC_GET_CURRENT_PROCESSOR_NUMBER  0x10
#define SVC_CLEAR_EVENT                   0x12
#define SVC_MAP_SHARED_MEMORY             0x13
#define SVC_UNMAP_SHARED_MEMORY           0x14
#define SVC_CREATE_TRANSFER_MEMORY        0x15
#define SVC_CLOSE_HANDLE                  0x16
#define SVC_RESET_SIGNAL                  0x17
#define SVC_WAIT_SYNCHRONIZATION          0x18
#define SVC_ARBITRATE_LOCK                0x1A
#define SVC_ARBITRATE_UNLOCK              0x1B
#define SVC_WAIT_PROCESS_WIDE_KEY_ATOMIC  0x1C
#define SVC_SIGNAL_PROCESS_WIDE_KEY       0x1D
#define SVC_GET_SYSTEM_TICK               0x1E
#define SVC_CONNECT_TO_NAMED_PORT         0x1F
#define SVC_SEND_SYNC_REQUEST             0x21
#define SVC_GET_THREAD_ID                 0x25
#define SVC_BREAK                         0x26
#define SVC_OUTPUT_DEBUG_STRING           0x27
#define SVC_GET_INFO                      0x29
#define SVC_MAP_PHYSICAL_MEMORY           0x2C
#define SVC_UNMAP_PHYSICAL_MEMORY         0x2D
#define SVC_SET_THREAD_ACTIVITY           0x32
#define SVC_GET_THREAD_CONTEXT_3          0x33

const char* horizon_svc_name(unsigned number)
{
    switch (number) {
    case SVC_SET_HEAP_SIZE:                return "svcSetHeapSize";
    case SVC_SET_MEMORY_ATTRIBUTE:         return "svcSetMemoryAttribute";
    case SVC_MAP_MEMORY:                   return "svcMapMemory";
    case SVC_UNMAP_MEMORY:                 return "svcUnmapMemory";
    case SVC_QUERY_MEMORY:                 return "svcQueryMemory";
    case SVC_EXIT_PROCESS:                 return "svcExitProcess";
    case SVC_CREATE_THREAD:                return "svcCreateThread";
    case SVC_START_THREAD:                 return "svcStartThread";
    case SVC_EXIT_THREAD:                  return "svcExitThread";
    case SVC_SLEEP_THREAD:                 return "svcSleepThread";
    case SVC_GET_THREAD_PRIORITY:          return "svcGetThreadPriority";
    case SVC_SET_THREAD_PRIORITY:          return "svcSetThreadPriority";
    case SVC_SET_THREAD_CORE_MASK:         return "svcSetThreadCoreMask";
    case SVC_GET_CURRENT_PROCESSOR_NUMBER: return "svcGetCurrentProcessorNumber";
    case SVC_CLEAR_EVENT:                  return "svcClearEvent";
    case SVC_MAP_SHARED_MEMORY:            return "svcMapSharedMemory";
    case SVC_UNMAP_SHARED_MEMORY:          return "svcUnmapSharedMemory";
    case SVC_CREATE_TRANSFER_MEMORY:       return "svcCreateTransferMemory";
    case SVC_CLOSE_HANDLE:                 return "svcCloseHandle";
    case SVC_RESET_SIGNAL:                 return "svcResetSignal";
    case SVC_WAIT_SYNCHRONIZATION:         return "svcWaitSynchronization";
    case SVC_ARBITRATE_LOCK:               return "svcArbitrateLock";
    case SVC_ARBITRATE_UNLOCK:             return "svcArbitrateUnlock";
    case SVC_WAIT_PROCESS_WIDE_KEY_ATOMIC: return "svcWaitProcessWideKeyAtomic";
    case SVC_SIGNAL_PROCESS_WIDE_KEY:      return "svcSignalProcessWideKey";
    case SVC_GET_SYSTEM_TICK:              return "svcGetSystemTick";
    case SVC_CONNECT_TO_NAMED_PORT:        return "svcConnectToNamedPort";
    case SVC_SEND_SYNC_REQUEST:            return "svcSendSyncRequest";
    case SVC_GET_THREAD_ID:                return "svcGetThreadId";
    case SVC_BREAK:                        return "svcBreak";
    case SVC_OUTPUT_DEBUG_STRING:          return "svcOutputDebugString";
    case SVC_GET_INFO:                     return "svcGetInfo";
    case SVC_MAP_PHYSICAL_MEMORY:          return "svcMapPhysicalMemory";
    case SVC_UNMAP_PHYSICAL_MEMORY:        return "svcUnmapPhysicalMemory";
    case SVC_SET_THREAD_ACTIVITY:          return "svcSetThreadActivity";
    case SVC_GET_THREAD_CONTEXT_3:         return "svcGetThreadContext3";
    default:                               return NULL;
    }
}

// ── the 32-bit marshalling ─────────────────────────────────────────────────
//
// See "ABI DERIVATIONS" in horizon_svc.h. Everything below implements D1
// (arguments in r0..r7, results in r0..) and D2 (64-bit values in even-aligned
// register pairs, low word first).

static inline uint64_t pair64(uint32_t lo, uint32_t hi)
{
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

static inline void set_pair64(minos_svc_frame* f, unsigned lo_reg, uint64_t value)
{
    f->r[lo_reg] = (uint32_t)value;
    f->r[lo_reg + 1] = (uint32_t)(value >> 32);
}

// The three services where D2's alignment rule is the only thing choosing
// between r4:r5 and r3:r4 for a 64-bit argument. Each reports its raw frame the
// first time it is reached -- once per process, three lines at most -- so the
// derivation can be checked against a real guest instead of believed.
static void log_ambiguous_once(bool* reported, const char* what,
                               const minos_svc_frame* f)
{
    if (*reported) return;
    *reported = true;
    vwine_logf("horizon svc: %s -- reading its 64-bit argument from r4:r5 "
               "(derivation D2 in horizon_svc.h; the alternative is r3:r4). "
               "First call only. r0-r7 = %08x %08x %08x %08x %08x %08x %08x "
               "%08x\n",
               what, f->r[0], f->r[1], f->r[2], f->r[3], f->r[4], f->r[5],
               f->r[6], f->r[7]);
}

// svcQueryMemory's MemoryInfo, in the ilp32 form -- see derivation D3. Field
// order follows Reference/horizon-linux/kernel/horizon/sys.c.
static void write_memory_info_ilp32(uint32_t addr, const hzn_memory_info* info)
{
    uint32_t* out = (uint32_t*)(uintptr_t)addr;
    out[0] = (uint32_t)info->addr;
    out[1] = (uint32_t)info->size;
    out[2] = info->state;
    out[3] = info->attr;
    out[4] = info->perm;
    out[5] = info->ipc_refcount;
    out[6] = info->device_refcount;
    out[7] = info->padding;
}

// ── the handler ────────────────────────────────────────────────────────────

static void svc_unimplemented(minos_svc_frame* frame)
{
    const char* name = horizon_svc_name(frame->imm);
    vwine_logf("horizon svc: guest executed svc #0x%02x (%s) at pc=0x%08x, "
               "which this front-end does not implement.\n"
               "  r0-r7  = %08x %08x %08x %08x %08x %08x %08x %08x\n"
               "  r8-r12 = %08x %08x %08x %08x %08x\n"
               "  sp=%08x lr=%08x cpsr=%08x\n"
               "Stopping here. Returning a Result for a service that did not "
               "run would leave the guest acting on an answer nobody "
               "computed.\n",
               frame->imm, name ? name : "not a Horizon SVC", frame->pc,
               frame->r[0], frame->r[1], frame->r[2], frame->r[3],
               frame->r[4], frame->r[5], frame->r[6], frame->r[7],
               frame->r[8], frame->r[9], frame->r[10], frame->r[11],
               frame->r[12], frame->sp, frame->lr, frame->cpsr);

    if (minos_current_user_abi && minos_current_user_abi->exit_fn)
        minos_current_user_abi->exit_fn(126);
}

void horizon_svc_handler(minos_svc_frame* frame)
{
    // A thread svcSetThreadActivity has paused stops here, on its way in. Under
    // cooperative scheduling this is the only point at which it can stop.
    horizon_kernel_check_paused();

    switch (frame->imm) {

    // ── memory ─────────────────────────────────────────────────────────────

    case SVC_SET_HEAP_SIZE: {
        uint32_t address = 0;
        const horizon_result rc = hzn_set_heap_size(frame->r[1], &address);
        frame->r[0] = rc;
        if (rc == HZN_RESULT_SUCCESS) frame->r[1] = address;
        break;
    }

    case SVC_SET_MEMORY_ATTRIBUTE:
        frame->r[0] = hzn_set_memory_attribute(frame->r[0], frame->r[1],
                                               frame->r[2], frame->r[3]);
        break;

    case SVC_MAP_MEMORY:
        frame->r[0] = hzn_map_memory(frame->r[0], frame->r[1], frame->r[2]);
        break;

    case SVC_UNMAP_MEMORY:
        frame->r[0] = hzn_unmap_memory(frame->r[0], frame->r[1], frame->r[2]);
        break;

    case SVC_QUERY_MEMORY: {
        hzn_memory_info info;
        uint32_t page_info = 0;
        const uint32_t out_ptr = frame->r[0];
        const horizon_result rc = hzn_query_memory(frame->r[2], &info, &page_info);
        if (rc == HZN_RESULT_SUCCESS && out_ptr != 0)
            write_memory_info_ilp32(out_ptr, &info);
        frame->r[0] = rc;
        frame->r[1] = page_info;
        break;
    }

    // ── process and threads ────────────────────────────────────────────────

    case SVC_EXIT_PROCESS:
        frame->r[0] = hzn_exit_process();
        break;

    case SVC_CREATE_THREAD: {
        uint32_t handle = 0;
        const horizon_result rc = hzn_create_thread(frame->r[1], frame->r[2],
                                                    frame->r[3],
                                                    (int32_t)frame->r[4],
                                                    (int32_t)frame->r[5],
                                                    &handle);
        frame->r[0] = rc;
        if (rc == HZN_RESULT_SUCCESS) frame->r[1] = handle;
        break;
    }

    case SVC_START_THREAD:
        frame->r[0] = hzn_start_thread(frame->r[0]);
        break;

    case SVC_EXIT_THREAD:
        // Does not return: it unwinds to the thread's host entry, abandoning
        // this frame. Nothing after this line runs for this thread.
        hzn_exit_thread();
        break;

    case SVC_SLEEP_THREAD:
        frame->r[0] = hzn_sleep_thread((int64_t)pair64(frame->r[0], frame->r[1]));
        break;

    case SVC_GET_THREAD_PRIORITY: {
        int32_t priority = 0;
        const horizon_result rc = hzn_get_thread_priority(frame->r[1], &priority);
        frame->r[0] = rc;
        if (rc == HZN_RESULT_SUCCESS) frame->r[1] = (uint32_t)priority;
        break;
    }

    case SVC_SET_THREAD_PRIORITY:
        frame->r[0] = hzn_set_thread_priority(frame->r[0], (int32_t)frame->r[1]);
        break;

    case SVC_SET_THREAD_CORE_MASK:
        // The mask is x2 on AArch64, so it lands in r2:r3 under both candidate
        // rules -- no ambiguity here.
        frame->r[0] = hzn_set_thread_core_mask(frame->r[0], (int32_t)frame->r[1],
                                               pair64(frame->r[2], frame->r[3]));
        break;

    case SVC_GET_CURRENT_PROCESSOR_NUMBER:
        // Returns the number itself, not a Result -- horizon-linux's
        // HSYSCALL_DEFINE0 returns raw_smp_processor_id() directly.
        frame->r[0] = hzn_get_current_processor_number();
        break;

    case SVC_GET_THREAD_ID: {
        uint64_t id = 0;
        const horizon_result rc = hzn_get_thread_id(frame->r[1], &id);
        frame->r[0] = rc;
        if (rc == HZN_RESULT_SUCCESS) set_pair64(frame, 1, id);
        break;
    }

    case SVC_SET_THREAD_ACTIVITY:
        frame->r[0] = hzn_set_thread_activity(frame->r[0], frame->r[1]);
        break;

    // ── handles and waiting ────────────────────────────────────────────────

    case SVC_CLOSE_HANDLE:
        frame->r[0] = hzn_close_handle(frame->r[0]);
        break;

    case SVC_RESET_SIGNAL:
        frame->r[0] = hzn_reset_signal(frame->r[0]);
        break;

    case SVC_CLEAR_EVENT:
        frame->r[0] = hzn_clear_event(frame->r[0]);
        break;

    case SVC_WAIT_SYNCHRONIZATION: {
        static bool reported = false;
        log_ambiguous_once(&reported, "svcWaitSynchronization", frame);

        int32_t index = 0;
        const horizon_result rc =
            hzn_wait_synchronization((const uint32_t*)(uintptr_t)frame->r[1],
                                     (int32_t)frame->r[2],
                                     (int64_t)pair64(frame->r[4], frame->r[5]),
                                     &index);
        frame->r[0] = rc;
        if (rc == HZN_RESULT_SUCCESS) frame->r[1] = (uint32_t)index;
        break;
    }

    case SVC_ARBITRATE_LOCK:
        frame->r[0] = hzn_arbitrate_lock(frame->r[0], frame->r[1], frame->r[2]);
        break;

    case SVC_ARBITRATE_UNLOCK:
        frame->r[0] = hzn_arbitrate_unlock(frame->r[0]);
        break;

    case SVC_WAIT_PROCESS_WIDE_KEY_ATOMIC: {
        static bool reported = false;
        log_ambiguous_once(&reported, "svcWaitProcessWideKeyAtomic", frame);

        // Argument order is (key, tag, self, timeout) -- x0 is the condition
        // variable and x1 the mutex, per wait_process_wide_key_atomic in
        // Reference/horizon-linux/kernel/horizon/sys.c. The kernel entry point
        // here takes them the other way round, hence the swap.
        frame->r[0] = hzn_wait_process_wide_key_atomic(
            frame->r[1], frame->r[0], frame->r[2],
            (int64_t)pair64(frame->r[4], frame->r[5]));
        break;
    }

    case SVC_SIGNAL_PROCESS_WIDE_KEY:
        frame->r[0] = hzn_signal_process_wide_key(frame->r[0], (int32_t)frame->r[1]);
        break;

    // ── shared and transfer memory ─────────────────────────────────────────

    case SVC_MAP_SHARED_MEMORY:
        frame->r[0] = hzn_map_shared_memory(frame->r[0], frame->r[1], frame->r[2],
                                            frame->r[3]);
        break;

    case SVC_UNMAP_SHARED_MEMORY:
        frame->r[0] = hzn_unmap_shared_memory(frame->r[0], frame->r[1], frame->r[2]);
        break;

    case SVC_CREATE_TRANSFER_MEMORY: {
        uint32_t handle = 0;
        const horizon_result rc = hzn_create_transfer_memory(
            frame->r[1], frame->r[2], frame->r[3], &handle);
        frame->r[0] = rc;
        if (rc == HZN_RESULT_SUCCESS) frame->r[1] = handle;
        break;
    }

    // ── time ───────────────────────────────────────────────────────────────

    case SVC_GET_SYSTEM_TICK:
        // Also a bare value rather than a Result.
        set_pair64(frame, 0, hzn_get_system_tick());
        break;

    // ── IPC ────────────────────────────────────────────────────────────────

    case SVC_CONNECT_TO_NAMED_PORT: {
        uint32_t handle = 0;
        // The name is a NUL-terminated string in guest memory. Horizon caps
        // port names at 11 characters plus the terminator; a longer one is
        // truncated here rather than read unbounded, and will then simply not
        // match.
        char name[12] = {0};
        const char* guest_name = (const char*)(uintptr_t)frame->r[1];
        if (guest_name) {
            for (size_t i = 0; i + 1 < sizeof(name); ++i) {
                name[i] = guest_name[i];
                if (name[i] == '\0') break;
            }
            name[sizeof(name) - 1] = '\0';
        }
        const horizon_result rc = horizon_ipc_connect_to_named_port(
            guest_name ? name : NULL, &handle);
        frame->r[0] = rc;
        if (rc == HZN_RESULT_SUCCESS) frame->r[1] = handle;
        break;
    }

    case SVC_SEND_SYNC_REQUEST:
        frame->r[0] = horizon_ipc_send_sync_request(frame->r[0]);
        break;

    // ── diagnostics ────────────────────────────────────────────────────────

    case SVC_OUTPUT_DEBUG_STRING:
        frame->r[0] = hzn_output_debug_string(frame->r[0], frame->r[1]);
        break;

    case SVC_BREAK:
        frame->r[0] = hzn_break(frame->r[0], frame->r[1], frame->r[2]);
        break;

    case SVC_GET_INFO: {
        static bool reported = false;
        log_ambiguous_once(&reported, "svcGetInfo", frame);

        uint64_t value = 0;
        const horizon_result rc = hzn_get_info(frame->r[1], frame->r[2],
                                               pair64(frame->r[4], frame->r[5]),
                                               &value);
        frame->r[0] = rc;
        if (rc == HZN_RESULT_SUCCESS) set_pair64(frame, 1, value);
        break;
    }

    default:
        svc_unimplemented(frame);
        break;
    }

    // TPIDRURO is CPU-wide and MVII does not save it across a context switch,
    // so any SVC that let another thread run has left it pointing at that
    // thread's TLS. Put this thread's back before returning to it. See note 2
    // in horizon_kernel.h.
    horizon_kernel_reload_thread_pointer();
}

// ── installation ───────────────────────────────────────────────────────────

bool horizon_svc_install(void)
{
    const minos_user_abi* abi = minos_current_user_abi;
    if (!abi || !abi->install_svc_handler_fn) {
        vwine_logf("horizon svc: this MVII build has no SVC-handler hook. A "
                   "Horizon guest talks to its kernel exclusively through "
                   "`svc`, so without the hook there is nothing to service and "
                   "the vector stays the fatal trap it is.\n");
        return false;
    }

    const long rc = abi->install_svc_handler_fn(&horizon_svc_handler);
    if (rc != 0) {
        vwine_logf("horizon svc: MVII refused the SVC handler (%ld)\n", rc);
        return false;
    }
    return true;
}

void horizon_svc_uninstall(void)
{
    const minos_user_abi* abi = minos_current_user_abi;
    if (abi && abi->install_svc_handler_fn) abi->install_svc_handler_fn(NULL);
}
