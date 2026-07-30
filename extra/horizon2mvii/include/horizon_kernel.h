// horizon_kernel.h — the Horizon kernel, reimplemented on MVII.
//
// This is the WINE half of the front-end. The guest's ARM instructions run on
// the Cortex-A7 untranslated (see horizon_image.h); what does not exist on the
// device is the kernel those instructions call, so it is rebuilt here on MVII's
// own primitives: threads from minos_user_thread_create, memory from vwine_map,
// time from the MVII clock.
//
// Every operation below is named after the SVC it serves and is checked against
// Reference/horizon-linux/kernel/horizon/sys.c, which is a real implementation
// of the same kernel interface. Where MVII cannot provide something -- address
// aliasing, principally -- the operation refuses and says why. It never returns
// a plausible-looking value it did not compute.
//
// ── two things that are not obvious ────────────────────────────────────────
//
// 1. SCHEDULING IS COOPERATIVE. MVII never preempts. A guest thread that spins
//    without entering the kernel has hung the process, not merely slowed it, so
//    every blocking operation here yields on its first failed attempt rather
//    than after a spin count. Horizon guests do reach the kernel (their locks
//    are svcArbitrateLock, their sleeps are svcSleepThread), so this works --
//    but it is the reason a "fast path" that avoids the SVC would be wrong.
//
// 2. THREAD-LOCAL STORAGE IS A CPU REGISTER WE OWN. A Horizon thread finds its
//    TLS -- and therefore its IPC command buffer -- through TPIDRURO. That
//    register is banked once for the whole CPU and MVII's scheduler does not
//    save it, exactly like SP_usr (see minos_svc.h). It stays correct for the
//    same reason: a guest thread can only stop running inside the SVC handler,
//    and the handler restores TPIDRURO from the resuming thread's own record
//    before it returns. Nothing else in MVII touches the register -- the
//    toolchain is built -femulated-tls -- so there is no second owner to race.

#ifndef HORIZON_KERNEL_H
#define HORIZON_KERNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "horizon_image.h"
#include "horizon_result.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── limits ─────────────────────────────────────────────────────────────────
//
// Fixed rather than grown: the whole front-end is one MVII process with one
// allocator, and a table that reallocates under a guest holding a pointer into
// it is a class of bug not worth having. Hitting a limit is reported by name.
#define HZN_MAX_HANDLES 128
#define HZN_MAX_THREADS 32
#define HZN_MAX_REGIONS 64

// Horizon's per-thread TLS block. The first 0x100 bytes are the IPC command
// buffer that svcSendSyncRequest reads; the rest is the guest's own.
#define HZN_TLS_BLOCK_SIZE 0x200u
#define HZN_IPC_BUFFER_SIZE 0x100u

// The two handles the guest may use without ever having been given them.
#define HZN_HANDLE_INVALID      0x00000000u
#define HZN_HANDLE_CURRENT_THREAD  0xFFFF8000u
#define HZN_HANDLE_CURRENT_PROCESS 0xFFFF8001u

// ── objects ────────────────────────────────────────────────────────────────

typedef enum hzn_object_kind {
    HZN_OBJECT_NONE = 0,
    HZN_OBJECT_THREAD,
    HZN_OBJECT_EVENT,
    HZN_OBJECT_SESSION,
    HZN_OBJECT_SHARED_MEMORY,
    HZN_OBJECT_TRANSFER_MEMORY,
} hzn_object_kind;

// Memory states, as svcQueryMemory reports them.
typedef enum hzn_memory_state {
    HZN_MEMORY_STATE_FREE = 0x00,
    HZN_MEMORY_STATE_CODE = 0x03,
    HZN_MEMORY_STATE_CODE_DATA = 0x04,
    HZN_MEMORY_STATE_NORMAL = 0x05,
    HZN_MEMORY_STATE_STACK = 0x0B,
    HZN_MEMORY_STATE_THREAD_LOCAL = 0x0C,
    HZN_MEMORY_STATE_TRANSFERRED = 0x0D,
    HZN_MEMORY_STATE_INACCESSIBLE = 0x10,
} hzn_memory_state;

typedef enum hzn_memory_permission {
    HZN_PERM_NONE = 0,
    HZN_PERM_READ = 1u << 0,
    HZN_PERM_WRITE = 1u << 1,
    HZN_PERM_EXECUTE = 1u << 2,
    HZN_PERM_READ_WRITE = HZN_PERM_READ | HZN_PERM_WRITE,
    HZN_PERM_READ_EXECUTE = HZN_PERM_READ | HZN_PERM_EXECUTE,
} hzn_memory_permission;

// What svcQueryMemory writes into the guest's MemoryInfo. The 64-bit fields are
// 64-bit even on AArch32: the structure is defined by the kernel ABI, not by
// the guest's pointer width.
typedef struct hzn_memory_info {
    uint64_t addr;
    uint64_t size;
    uint32_t state;
    uint32_t attr;
    uint32_t perm;
    uint32_t ipc_refcount;
    uint32_t device_refcount;
    uint32_t padding;
} hzn_memory_info;

// ── lifecycle ──────────────────────────────────────────────────────────────

// Bring the kernel up around an already-loaded module.
//
// `heap_reserve` is the maximum the guest's heap may ever reach. It is reserved
// in one piece here rather than grown on demand because Horizon guarantees the
// heap base address never moves, and MVII has no way to extend a mapping in
// place -- so a later grow would have to relocate it under a guest holding
// pointers into it. svcSetHeapSize moves a boundary inside this reservation and
// refuses, by name, anything past it.
//
// Returns false with the reason logged.
bool horizon_kernel_init(const horizon_module* module, size_t heap_reserve);
void horizon_kernel_shutdown(void);

// True once the guest has asked to exit (svcExitProcess), and the code it gave.
bool horizon_kernel_exit_requested(void);
int  horizon_kernel_exit_code(void);

// Run `module`'s entry point as the guest's main thread and return when it
// exits. This is the branch into guest code.
horizon_result horizon_kernel_run_main_thread(const horizon_module* module);

// ── the SVC implementations ────────────────────────────────────────────────
//
// Plain C signatures in Horizon's own terms; horizon_svc.c does the register
// marshalling and nothing else. Out-parameters are written only on success,
// which is what the guest's own wrappers assume.

horizon_result hzn_set_heap_size(uint32_t size, uint32_t* out_address);
horizon_result hzn_set_memory_attribute(uint32_t addr, uint32_t size, uint32_t mask,
                                        uint32_t value);
horizon_result hzn_map_memory(uint32_t dst, uint32_t src, uint32_t size);
horizon_result hzn_unmap_memory(uint32_t dst, uint32_t src, uint32_t size);
horizon_result hzn_query_memory(uint32_t addr, hzn_memory_info* out_info,
                                uint32_t* out_page_info);

horizon_result hzn_exit_process(void);

horizon_result hzn_create_thread(uint32_t entry, uint32_t arg, uint32_t stack_top,
                                 int32_t priority, int32_t core, uint32_t* out_handle);
horizon_result hzn_start_thread(uint32_t handle);
horizon_result hzn_exit_thread(void);   // never returns
horizon_result hzn_sleep_thread(int64_t nanoseconds);
horizon_result hzn_get_thread_priority(uint32_t handle, int32_t* out_priority);
horizon_result hzn_set_thread_priority(uint32_t handle, int32_t priority);
horizon_result hzn_set_thread_core_mask(uint32_t handle, int32_t core, uint64_t mask);
uint32_t       hzn_get_current_processor_number(void);
horizon_result hzn_get_thread_id(uint32_t handle, uint64_t* out_id);
horizon_result hzn_set_thread_activity(uint32_t handle, uint32_t activity);

horizon_result hzn_close_handle(uint32_t handle);
horizon_result hzn_reset_signal(uint32_t handle);
horizon_result hzn_clear_event(uint32_t handle);
horizon_result hzn_wait_synchronization(const uint32_t* handles, int32_t count,
                                        int64_t timeout_ns, int32_t* out_index);

horizon_result hzn_arbitrate_lock(uint32_t owner_handle, uint32_t tag_addr,
                                  uint32_t requester_handle);
horizon_result hzn_arbitrate_unlock(uint32_t tag_addr);
horizon_result hzn_wait_process_wide_key_atomic(uint32_t tag_addr, uint32_t key_addr,
                                                uint32_t self_handle, int64_t timeout_ns);
horizon_result hzn_signal_process_wide_key(uint32_t key_addr, int32_t count);

uint64_t       hzn_get_system_tick(void);
uint64_t       hzn_get_system_tick_frequency(void);

horizon_result hzn_map_shared_memory(uint32_t handle, uint32_t addr, uint32_t size,
                                     uint32_t perm);
horizon_result hzn_unmap_shared_memory(uint32_t handle, uint32_t addr, uint32_t size);
horizon_result hzn_create_transfer_memory(uint32_t addr, uint32_t size, uint32_t perm,
                                          uint32_t* out_handle);

horizon_result hzn_output_debug_string(uint32_t addr, uint32_t length);
horizon_result hzn_break(uint32_t reason, uint32_t addr, uint32_t size);
horizon_result hzn_get_info(uint32_t info_type, uint32_t handle, uint64_t sub_type,
                            uint64_t* out_value);

// ── what the SVC dispatcher needs from the kernel ──────────────────────────
//
// Both are called by horizon_svc.c around every SVC, and both exist because of
// the two notes at the top of this file.

// Put the resuming thread's TLS pointer back in TPIDRURO. Called on the way out
// of the handler, after any operation that may have run another thread. See
// note 2: the register is CPU-wide and MVII does not save it, so the handler is
// the only place that can restore it.
void horizon_kernel_reload_thread_pointer(void);

// Hold the calling thread here while svcSetThreadActivity has it paused. Called
// on the way in. See note 1: cooperative scheduling means an SVC is the only
// point at which a thread can be stopped at all.
void horizon_kernel_check_paused(void);

// ── what the IPC layer needs from the kernel ───────────────────────────────

// The calling thread's IPC command buffer -- the first 0x100 bytes of its TLS
// block. NULL if called from a thread the kernel does not know about, which is
// a front-end bug rather than a guest one.
void* horizon_kernel_current_ipc_buffer(void);

// A kernel event, for services that hand the guest something to wait on (a
// display's vsync, an input sample). `auto_clear` events clear themselves when
// a wait consumes them, which is what Horizon calls an autoclear event; the
// others stay signalled until svcClearEvent.
//
// Returns HZN_HANDLE_INVALID when the handle table is full, reported by name.
uint32_t horizon_event_create(bool auto_clear);
void     horizon_event_signal(uint32_t handle);

// Register a kernel object and return its handle, or HZN_HANDLE_INVALID when
// the table is full (reported by name). `payload` is interpreted per kind and
// owned by the caller.
uint32_t horizon_handle_create(hzn_object_kind kind, void* payload);
void*    horizon_handle_lookup(uint32_t handle, hzn_object_kind expected_kind);

// A named guest region, for svcQueryMemory. Registered by whoever allocates it.
bool horizon_region_add(uintptr_t base, size_t size, hzn_memory_state state,
                        hzn_memory_permission perm);
void horizon_region_remove(uintptr_t base);

#ifdef __cplusplus
}
#endif

#endif  // HORIZON_KERNEL_H
