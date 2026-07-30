#include "horizon_kernel.h"

#include <string.h>

#include "horizon_ipc.h"
#include "minos_svc.h"
#include "minos_user_abi.h"
#include "vwine/vwine_log.h"
#include "vwine/vwine_mem.h"

// Defined by R-Dash and filled in by Dash/armv7/crt.s from the vtable MVII
// hands the process in r12. Declared, never defined here.
extern const minos_user_abi* minos_current_user_abi;

// ── the clock ──────────────────────────────────────────────────────────────
//
// Horizon's system tick runs at 19.2 MHz and guests hard-code that constant to
// convert ticks to nanoseconds (libnx's armTicksToNs). So the frequency
// reported here is not a choice: the number below has to be the real one or
// every timing calculation the guest makes is scaled wrong. What is derived is
// the tick VALUE, from MVII's microsecond clock.
#define HZN_TICK_HZ 19200000ull

static const minos_user_abi* g_abi;

// ── objects ────────────────────────────────────────────────────────────────

typedef struct hzn_context {
    uint32_t r[8];   // r4-r11
    uint32_t sp;
    uint32_t lr;
} hzn_context;

typedef enum hzn_thread_state {
    HZN_THREAD_CREATED = 0,
    HZN_THREAD_RUNNING,
    HZN_THREAD_FINISHED,
} hzn_thread_state;

typedef struct hzn_thread {
    bool     used;
    uint32_t handle;
    uint64_t id;
    uintptr_t host_thread;    // the MVII thread, 0 for the main one
    uint32_t entry;
    uint32_t guest_arg;
    uint32_t stack_top;
    int32_t  priority;
    int32_t  core;
    uint8_t* tls;
    volatile uint32_t state;
    volatile uint32_t paused;
    // Where hzn_exit_thread returns to. Captured in the host thread function
    // just before it branches into the guest, so unwinding is a stack-pointer
    // restore rather than an unwind -- see the comment on hzn_exit_thread.
    hzn_context exit_context;
} hzn_thread;

typedef struct hzn_event {
    bool used;
    volatile uint32_t signalled;
    bool auto_clear;
} hzn_event;

#define HZN_MAX_EVENTS 32

typedef struct hzn_handle_entry {
    hzn_object_kind kind;
    void*           payload;
} hzn_handle_entry;

typedef struct hzn_region {
    bool                  used;
    uintptr_t             base;
    size_t                size;
    hzn_memory_state      state;
    hzn_memory_permission perm;
    uint32_t              attr;
} hzn_region;

// A process-wide key (a guest condition variable). Keyed by the guest address
// the guest passed, which is what makes it "process-wide": two threads naming
// the same address name the same key.
typedef struct hzn_key {
    bool     used;
    uint32_t addr;
    uint32_t waiters;
    int32_t  to_wake;
} hzn_key;

#define HZN_MAX_KEYS 32

static hzn_handle_entry g_handles[HZN_MAX_HANDLES];
static hzn_thread       g_threads[HZN_MAX_THREADS];
static hzn_region       g_regions[HZN_MAX_REGIONS];
static hzn_key          g_keys[HZN_MAX_KEYS];
static hzn_event        g_events[HZN_MAX_EVENTS];

static uint64_t g_next_thread_id = 1;

static vwine_mapping g_heap;
static size_t        g_heap_committed;
static vwine_mapping g_tls_area;
static vwine_mapping g_main_stack;

static uintptr_t g_code_base, g_code_size;
static uintptr_t g_stack_base, g_stack_size;

static volatile bool g_exit_requested;
static volatile int  g_exit_code;

// ── context save/restore ───────────────────────────────────────────────────
//
// setjmp/longjmp in eleven instructions, because svcExitThread has to leave the
// SVC handler without returning through it and the freestanding ARM sysroot has
// no <setjmp.h>. Both the save site and the restore site run in SVC mode on the
// same host thread's stack -- the save is in the thread's host entry function,
// the restore is inside the SVC handler further up that same stack -- so this
// is a stack-pointer rewind, and abandoning the vector's exception frame along
// the way is exactly what is wanted: the thread it belonged to is over.
//
// STM/LDM with SP or LR in the register list is deprecated on ARMv7, so the two
// are stored individually.

__attribute__((naked, noinline))
static int horizon_context_save(hzn_context* ctx)
{
    __asm__ volatile(
        "stm    r0!, {r4-r11}\n\t"
        "str    sp, [r0], #4\n\t"
        "str    lr, [r0]\n\t"
        "mov    r0, #0\n\t"
        "bx     lr");
}

__attribute__((naked, noinline, noreturn))
static void horizon_context_restore(hzn_context* ctx, int value)
{
    __asm__ volatile(
        "ldm    r0!, {r4-r11}\n\t"
        "ldr    sp, [r0], #4\n\t"
        "ldr    lr, [r0]\n\t"
        "mov    r0, r1\n\t"
        "bx     lr");
}

// ── the thread pointer ─────────────────────────────────────────────────────
//
// TPIDRURO is where a Horizon thread finds its TLS block, and therefore its IPC
// command buffer. Writing it needs PL1, which a Virtua process has; reading it
// works at PL0 too, which is what the guest does.

static inline void horizon_set_thread_pointer(const void* tls)
{
    __asm__ volatile("mcr p15, 0, %0, c13, c0, 3" :: "r"(tls) : "memory");
}

// ── small helpers ──────────────────────────────────────────────────────────

static void horizon_yield(void)
{
    if (g_abi && g_abi->thread_yield_fn) g_abi->thread_yield_fn();
}

static uint64_t horizon_now_us(void)
{
    if (!g_abi || !g_abi->gettimeofday_fn) return 0;
    struct { long tv_sec; long tv_usec; } tv = {0, 0};
    if (g_abi->gettimeofday_fn(&tv, NULL) != 0) return 0;
    if (tv.tv_sec < 0 || tv.tv_usec < 0) return 0;
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}

static hzn_thread* horizon_thread_by_host(uintptr_t host)
{
    // Zero is the "not running yet" marker on a created-but-unstarted thread,
    // so it must never match -- otherwise the first such thread in the table
    // would answer for whoever asked.
    if (host == 0) return NULL;
    for (size_t i = 0; i < HZN_MAX_THREADS; ++i)
        if (g_threads[i].used && g_threads[i].host_thread == host) return &g_threads[i];
    return NULL;
}

static hzn_thread* horizon_thread_current(void)
{
    const uintptr_t self = (g_abi && g_abi->thread_self_fn) ? g_abi->thread_self_fn() : 0;
    return horizon_thread_by_host(self);
}

static hzn_thread* horizon_thread_from_handle(uint32_t handle)
{
    if (handle == HZN_HANDLE_CURRENT_THREAD) return horizon_thread_current();
    return (hzn_thread*)horizon_handle_lookup(handle, HZN_OBJECT_THREAD);
}

// Is `addr`..`addr+size` inside a region this kernel handed out? Guest and host
// share one flat address space, so a bad guest pointer would fault the
// front-end rather than the guest; this turns that into a Result the guest can
// see, which is also what the real kernel returns.
static bool horizon_addr_valid(uint32_t addr, size_t size)
{
    if (addr == 0 || size == 0) return false;
    const uintptr_t start = (uintptr_t)addr;
    if (start + size < start) return false;
    for (size_t i = 0; i < HZN_MAX_REGIONS; ++i) {
        if (!g_regions[i].used) continue;
        if (start >= g_regions[i].base &&
            start + size <= g_regions[i].base + g_regions[i].size)
            return true;
    }
    return false;
}

// ── handles ────────────────────────────────────────────────────────────────

uint32_t horizon_handle_create(hzn_object_kind kind, void* payload)
{
    for (size_t i = 0; i < HZN_MAX_HANDLES; ++i) {
        if (g_handles[i].kind != HZN_OBJECT_NONE) continue;
        g_handles[i].kind = kind;
        g_handles[i].payload = payload;
        // +1 so that index 0 is not handle 0, which Horizon reserves for
        // "invalid" and every guest tests against.
        return (uint32_t)(i + 1);
    }
    vwine_logf("horizon: the handle table is full (%u handles); the guest asked "
               "for one more of kind %d\n", (unsigned)HZN_MAX_HANDLES, (int)kind);
    return HZN_HANDLE_INVALID;
}

void* horizon_handle_lookup(uint32_t handle, hzn_object_kind expected_kind)
{
    if (handle == HZN_HANDLE_INVALID || handle > HZN_MAX_HANDLES) return NULL;
    hzn_handle_entry* e = &g_handles[handle - 1];
    if (e->kind == HZN_OBJECT_NONE) return NULL;
    if (expected_kind != HZN_OBJECT_NONE && e->kind != expected_kind) return NULL;
    return e->payload;
}

// ── regions ────────────────────────────────────────────────────────────────

bool horizon_region_add(uintptr_t base, size_t size, hzn_memory_state state,
                        hzn_memory_permission perm)
{
    for (size_t i = 0; i < HZN_MAX_REGIONS; ++i) {
        if (g_regions[i].used) continue;
        g_regions[i].used = true;
        g_regions[i].base = base;
        g_regions[i].size = size;
        g_regions[i].state = state;
        g_regions[i].perm = perm;
        g_regions[i].attr = 0;
        return true;
    }
    vwine_logf("horizon: the memory-region table is full (%u entries); "
               "svcQueryMemory will not see the region at %p\n",
               (unsigned)HZN_MAX_REGIONS, (void*)base);
    return false;
}

void horizon_region_remove(uintptr_t base)
{
    for (size_t i = 0; i < HZN_MAX_REGIONS; ++i)
        if (g_regions[i].used && g_regions[i].base == base) g_regions[i].used = false;
}

// ── init / shutdown ────────────────────────────────────────────────────────

// Main-thread stack. Horizon's own main thread gets 1 MiB by default and
// nothing here is deeper than the guest makes it.
#define HZN_MAIN_STACK_SIZE (1u << 20)

bool horizon_kernel_init(const horizon_module* module, size_t heap_reserve)
{
    if (!module) return false;

    g_abi = minos_current_user_abi;
    if (!g_abi || !g_abi->thread_create_fn || !g_abi->thread_self_fn ||
        !g_abi->thread_yield_fn || !g_abi->gettimeofday_fn ||
        !g_abi->install_svc_handler_fn) {
        vwine_logf("horizon: the MVII user ABI is missing something this kernel "
                   "stands on (threads, clock, or the SVC hook). An older "
                   "PowerEngine cannot host a Horizon guest -- the SVC hook in "
                   "particular is the whole mechanism.\n");
        return false;
    }

    memset(g_handles, 0, sizeof(g_handles));
    memset(g_threads, 0, sizeof(g_threads));
    memset(g_regions, 0, sizeof(g_regions));
    memset(g_keys, 0, sizeof(g_keys));
    memset(g_events, 0, sizeof(g_events));
    g_exit_requested = false;
    g_exit_code = 0;
    g_next_thread_id = 1;

    g_code_base = (uintptr_t)module->image.mapping.base;
    g_code_size = module->image.mapping.length;
    horizon_region_add(g_code_base, g_code_size, HZN_MEMORY_STATE_CODE,
                       HZN_PERM_READ_EXECUTE);

    // The heap, reserved once. See the note in horizon_kernel.h on why this is
    // not grown on demand.
    if (heap_reserve > 0) {
        const int rc = vwine_map(heap_reserve, &g_heap);
        if (rc != 0) {
            vwine_logf("horizon: could not reserve a %zu-byte heap (%d). The "
                       "guest's first svcSetHeapSize would fail, so stopping "
                       "here instead.\n", heap_reserve, rc);
            return false;
        }
        g_heap_committed = 0;
    }

    // One TLS area for every thread the kernel can create, contiguous so
    // svcQueryMemory can describe it as the single THREAD_LOCAL region Horizon
    // reports.
    if (vwine_map(HZN_TLS_BLOCK_SIZE * HZN_MAX_THREADS, &g_tls_area) != 0) {
        vwine_logf("horizon: could not reserve the thread-local storage area\n");
        return false;
    }
    horizon_region_add((uintptr_t)g_tls_area.base, g_tls_area.length,
                       HZN_MEMORY_STATE_THREAD_LOCAL, HZN_PERM_READ_WRITE);

    if (vwine_map(HZN_MAIN_STACK_SIZE, &g_main_stack) != 0) {
        vwine_logf("horizon: could not reserve the guest's main-thread stack\n");
        return false;
    }
    g_stack_base = (uintptr_t)g_main_stack.base;
    g_stack_size = g_main_stack.length;
    horizon_region_add(g_stack_base, g_stack_size, HZN_MEMORY_STATE_STACK,
                       HZN_PERM_READ_WRITE);

    return true;
}

void horizon_kernel_shutdown(void)
{
    vwine_unmap(&g_heap);
    vwine_unmap(&g_tls_area);
    vwine_unmap(&g_main_stack);
    memset(g_handles, 0, sizeof(g_handles));
    memset(g_threads, 0, sizeof(g_threads));
    memset(g_regions, 0, sizeof(g_regions));
}

bool horizon_kernel_exit_requested(void) { return g_exit_requested; }
int  horizon_kernel_exit_code(void) { return g_exit_code; }

void* horizon_kernel_current_ipc_buffer(void)
{
    const hzn_thread* t = horizon_thread_current();
    if (!t) return NULL;

    // Cross-check the table against the register, because they are two views of
    // the same fact and only one of them is the guest's. The guest finds its
    // command buffer through TPIDRURO; this function answers from the thread
    // table. If they ever disagree, every IPC call reads its arguments from one
    // buffer and writes its reply into another, and nothing about the resulting
    // failure would point back here -- which is exactly why note 2 in
    // horizon_kernel.h is the riskiest assumption in this file. The check is one
    // coprocessor read on a path that is already an SVC, so it stays on.
    void* held;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(held));
    if (held != t->tls) {
        vwine_logf("horizon: TPIDRURO is %p but thread %llu's TLS block is at "
                   "%p. The register is CPU-wide and MVII's scheduler does not "
                   "save it, so something ran between the last SVC exit and this "
                   "one without going through horizon_kernel_reload_thread_"
                   "pointer. The guest's command buffer is not the one this "
                   "kernel would answer with, so stopping here rather than "
                   "servicing IPC against the wrong 0x100 bytes.\n",
                   held, (unsigned long long)t->id, (void*)t->tls);
        return NULL;
    }
    return t->tls;
}

// Called by the SVC handler on its way back to the guest. See the TLS note in
// horizon_kernel.h: TPIDRURO is CPU-wide and MVII does not save it, so the
// resuming thread's value has to be put back by the only code that knows which
// thread is resuming.
void horizon_kernel_reload_thread_pointer(void)
{
    const hzn_thread* t = horizon_thread_current();
    if (t && t->tls) horizon_set_thread_pointer(t->tls);
}

// Also called by the SVC handler, on entry: a paused thread is one
// svcSetThreadActivity stopped, and an SVC is the only place it can be held.
void horizon_kernel_check_paused(void)
{
    hzn_thread* t = horizon_thread_current();
    if (!t) return;
    while (t->paused && !g_exit_requested) horizon_yield();
}

// ── threads ────────────────────────────────────────────────────────────────

static hzn_thread* horizon_thread_alloc(void)
{
    for (size_t i = 0; i < HZN_MAX_THREADS; ++i) {
        if (g_threads[i].used) continue;
        memset(&g_threads[i], 0, sizeof(g_threads[i]));
        g_threads[i].used = true;
        g_threads[i].id = g_next_thread_id++;
        g_threads[i].tls = (uint8_t*)g_tls_area.base + i * HZN_TLS_BLOCK_SIZE;
        memset(g_threads[i].tls, 0, HZN_TLS_BLOCK_SIZE);
        return &g_threads[i];
    }
    vwine_logf("horizon: the thread table is full (%u threads)\n",
               (unsigned)HZN_MAX_THREADS);
    return NULL;
}

// The host side of a guest thread. Runs as an ordinary MVII thread; its only
// job is to become the guest.
static void* horizon_host_thread_main(void* arg)
{
    hzn_thread* t = (hzn_thread*)arg;
    t->host_thread = g_abi->thread_self_fn();
    t->state = HZN_THREAD_RUNNING;

    horizon_set_thread_pointer(t->tls);

    // Returns 0 here, and 1 when hzn_exit_thread unwinds back into it.
    if (horizon_context_save(&t->exit_context) == 0)
        minos_svc_enter_guest(t->entry, t->stack_top, t->guest_arg, t->handle);

    t->state = HZN_THREAD_FINISHED;
    return NULL;
}

horizon_result hzn_create_thread(uint32_t entry, uint32_t arg, uint32_t stack_top,
                                 int32_t priority, int32_t core, uint32_t* out_handle)
{
    if (entry == 0 || stack_top == 0) return HZN_RESULT_INVALID_ADDRESS;
    // Horizon's user priorities are 0..0x3F, low number = high priority.
    if (priority < 0 || priority > 0x3F) return HZN_RESULT_INVALID_PRIORITY;

    hzn_thread* t = horizon_thread_alloc();
    if (!t) return HZN_RESULT_OUT_OF_RESOURCE;

    t->entry = entry;
    t->guest_arg = arg;
    t->stack_top = stack_top;
    t->priority = priority;
    t->core = core;
    t->state = HZN_THREAD_CREATED;

    t->handle = horizon_handle_create(HZN_OBJECT_THREAD, t);
    if (t->handle == HZN_HANDLE_INVALID) {
        t->used = false;
        return HZN_RESULT_OUT_OF_HANDLES;
    }

    *out_handle = t->handle;
    return HZN_RESULT_SUCCESS;
}

horizon_result hzn_start_thread(uint32_t handle)
{
    hzn_thread* t = (hzn_thread*)horizon_handle_lookup(handle, HZN_OBJECT_THREAD);
    if (!t) return HZN_RESULT_INVALID_HANDLE;
    if (t->state != HZN_THREAD_CREATED) return HZN_RESULT_INVALID_STATE;

    // The host thread gets its OWN stack, separate from the guest stack the
    // guest passed to svcCreateThread. That is not redundancy: the host stack
    // becomes this thread's SP_svc, which is where its SVC frames land while
    // the guest runs on SP_usr. Sharing one stack for both would have the
    // exception frame overwrite whatever the guest had below its stack
    // pointer.
    const size_t host_stack = 64u * 1024u;
    // The out-parameter goes to a scratch: the thread records its own identity
    // from thread_self_fn, and that is the value horizon_thread_by_host matches
    // against. Letting thread_create_fn write the field too would mean two
    // writers for one word and two sources for a value that must agree.
    uintptr_t spawned = 0;
    const long rc = g_abi->thread_create_fn(&spawned,
                                           (uintptr_t)&horizon_host_thread_main,
                                           (uintptr_t)t, host_stack);
    if (rc != 0) {
        vwine_logf("horizon: MVII refused to spawn a thread for the guest (%ld)\n", rc);
        return HZN_RESULT_OUT_OF_RESOURCE;
    }
    return HZN_RESULT_SUCCESS;
}

horizon_result hzn_exit_thread(void)
{
    hzn_thread* t = horizon_thread_current();
    if (!t) {
        vwine_logf("horizon: svcExitThread from a thread this kernel does not "
                   "know about; that is a front-end bug, not a guest one\n");
        return HZN_RESULT_INVALID_STATE;
    }
    t->state = HZN_THREAD_FINISHED;
    // Leaves the SVC handler without returning through it -- see the comment on
    // horizon_context_save. Does not come back.
    horizon_context_restore(&t->exit_context, 1);
}

horizon_result hzn_sleep_thread(int64_t nanoseconds)
{
    // Horizon gives three negative values a meaning of their own, all of them
    // "give up the CPU now" in different flavours of hint. On a cooperative
    // scheduler with no priorities every one of them is a yield, and saying so
    // is more honest than pretending to distinguish them.
    if (nanoseconds <= 0) {
        horizon_yield();
        return HZN_RESULT_SUCCESS;
    }

    const uint64_t deadline_us = horizon_now_us() + (uint64_t)(nanoseconds / 1000);
    if (g_abi->sleep_until_us_fn) {
        // MVII clamps a single sleep to one second and expects the caller to
        // re-check, so this loops rather than trusting one call.
        while (!g_exit_requested && horizon_now_us() < deadline_us)
            g_abi->sleep_until_us_fn(deadline_us);
    } else {
        while (!g_exit_requested && horizon_now_us() < deadline_us) horizon_yield();
    }
    return HZN_RESULT_SUCCESS;
}

horizon_result hzn_get_thread_priority(uint32_t handle, int32_t* out_priority)
{
    const hzn_thread* t = horizon_thread_from_handle(handle);
    if (!t) return HZN_RESULT_INVALID_HANDLE;
    *out_priority = t->priority;
    return HZN_RESULT_SUCCESS;
}

horizon_result hzn_set_thread_priority(uint32_t handle, int32_t priority)
{
    hzn_thread* t = horizon_thread_from_handle(handle);
    if (!t) return HZN_RESULT_INVALID_HANDLE;
    if (priority < 0 || priority > 0x3F) return HZN_RESULT_INVALID_PRIORITY;
    // Recorded, not enforced: MVII's scheduler is cooperative and round-robin,
    // so there is no priority for this to set. The value is kept because
    // svcGetThreadPriority must return what was stored, and guests do read
    // back what they wrote.
    t->priority = priority;
    return HZN_RESULT_SUCCESS;
}

horizon_result hzn_set_thread_core_mask(uint32_t handle, int32_t core, uint64_t mask)
{
    hzn_thread* t = horizon_thread_from_handle(handle);
    if (!t) return HZN_RESULT_INVALID_HANDLE;
    (void)mask;
    t->core = core;
    return HZN_RESULT_SUCCESS;
}

uint32_t hzn_get_current_processor_number(void)
{
    // The J36 runs MVII's userland on one core. Reporting anything else would
    // be inventing a topology the guest could then try to spread work across.
    return 0;
}

horizon_result hzn_get_thread_id(uint32_t handle, uint64_t* out_id)
{
    const hzn_thread* t = horizon_thread_from_handle(handle);
    if (!t) return HZN_RESULT_INVALID_HANDLE;
    *out_id = t->id;
    return HZN_RESULT_SUCCESS;
}

horizon_result hzn_set_thread_activity(uint32_t handle, uint32_t activity)
{
    hzn_thread* t = horizon_thread_from_handle(handle);
    if (!t) return HZN_RESULT_INVALID_HANDLE;
    if (activity > 1) return HZN_RESULT_INVALID_ENUM_VALUE;
    if (t == horizon_thread_current()) return HZN_RESULT_BUSY;
    // Takes effect at the target thread's next SVC (see
    // horizon_kernel_check_paused). Under cooperative scheduling that is the
    // only place it can take effect at all, since nothing interrupts a running
    // thread to notice the flag.
    t->paused = (activity == 1);
    return HZN_RESULT_SUCCESS;
}

// ── the main thread ────────────────────────────────────────────────────────

horizon_result horizon_kernel_run_main_thread(const horizon_module* module)
{
    hzn_thread* t = horizon_thread_alloc();
    if (!t) return HZN_RESULT_OUT_OF_RESOURCE;

    t->entry = (uint32_t)module->image.entry;
    t->guest_arg = 0;
    // AAPCS wants the stack 8-byte aligned and full-descending, so the guest
    // starts at the top of the region.
    t->stack_top = (uint32_t)(g_stack_base + g_stack_size);
    t->priority = 0x2C;   // Horizon's default main-thread priority
    t->core = 0;
    t->state = HZN_THREAD_RUNNING;
    t->host_thread = g_abi->thread_self_fn();

    t->handle = horizon_handle_create(HZN_OBJECT_THREAD, t);
    if (t->handle == HZN_HANDLE_INVALID) return HZN_RESULT_OUT_OF_HANDLES;

    horizon_set_thread_pointer(t->tls);

    // Horizon hands the main thread (r0, r1) = (0, main thread handle) --
    // libnx's __nx_exception_entry reads them that way, and a homebrew NRO's
    // _start passes them straight to __libnx_init.
    if (horizon_context_save(&t->exit_context) == 0)
        minos_svc_enter_guest(t->entry, t->stack_top, 0, t->handle);

    t->state = HZN_THREAD_FINISHED;
    return HZN_RESULT_SUCCESS;
}

horizon_result hzn_exit_process(void)
{
    g_exit_requested = true;
    vwine_logf("horizon: guest called svcExitProcess\n");
    // Every other guest thread is parked inside its own SVC or running guest
    // code; there is no way to unwind them from here, and Horizon's own
    // ExitProcess does not try to. Ending the MVII process is the same
    // observable behaviour.
    if (g_abi && g_abi->exit_fn) g_abi->exit_fn(g_exit_code);
    return HZN_RESULT_SUCCESS;
}

// ── memory ─────────────────────────────────────────────────────────────────

horizon_result hzn_set_heap_size(uint32_t size, uint32_t* out_address)
{
    if ((size & (VWINE_PAGE_SIZE - 1u)) != 0) return HZN_RESULT_INVALID_SIZE;
    if (!g_heap.base) {
        vwine_logf("horizon: svcSetHeapSize, but no heap was reserved at "
                   "startup\n");
        return HZN_RESULT_OUT_OF_MEMORY;
    }
    if (size > g_heap.length) {
        vwine_logf("horizon: the guest asked for a %u-byte heap; %zu bytes were "
                   "reserved at startup and the reservation cannot grow "
                   "(Horizon guarantees the heap base never moves, and MVII "
                   "cannot extend a mapping in place). Raise the reservation.\n",
                   size, g_heap.length);
        return HZN_RESULT_OUT_OF_MEMORY;
    }

    horizon_region_remove((uintptr_t)g_heap.base);
    g_heap_committed = size;
    if (size > 0)
        horizon_region_add((uintptr_t)g_heap.base, size, HZN_MEMORY_STATE_NORMAL,
                           HZN_PERM_READ_WRITE);

    *out_address = (uint32_t)(uintptr_t)g_heap.base;
    return HZN_RESULT_SUCCESS;
}

horizon_result hzn_set_memory_attribute(uint32_t addr, uint32_t size, uint32_t mask,
                                        uint32_t value)
{
    if ((addr & (VWINE_PAGE_SIZE - 1u)) != 0) return HZN_RESULT_INVALID_ADDRESS;
    if (size == 0 || (size & (VWINE_PAGE_SIZE - 1u)) != 0) return HZN_RESULT_INVALID_SIZE;
    if ((mask | value) != mask) return HZN_RESULT_INVALID_COMBINATION;
    // Only the "uncached" attribute is a real request on hardware, and MVII maps
    // all of DRAM write-back with no per-page control, so honouring it is not
    // possible. Refuse rather than accept-and-ignore: a guest that sets
    // uncached is about to share that memory with a device and expects the
    // writes to land.
    if ((mask & 0x8u) != 0) {
        vwine_logf("horizon: svcSetMemoryAttribute asked for uncached memory at "
                   "0x%08x. MVII maps DRAM write-back with no per-page control, "
                   "so this cannot be honoured and accepting it would be a lie "
                   "the guest acts on.\n", addr);
        return HZN_RESULT_INVALID_COMBINATION;
    }
    if (!horizon_addr_valid(addr, size)) return HZN_RESULT_INVALID_CURRENT_MEMORY;

    for (size_t i = 0; i < HZN_MAX_REGIONS; ++i) {
        if (!g_regions[i].used) continue;
        if ((uintptr_t)addr < g_regions[i].base ||
            (uintptr_t)addr + size > g_regions[i].base + g_regions[i].size) continue;
        g_regions[i].attr = (g_regions[i].attr & ~mask) | (value & mask);
        return HZN_RESULT_SUCCESS;
    }
    return HZN_RESULT_INVALID_CURRENT_MEMORY;
}

// The one thing MVII genuinely cannot do.
static horizon_result horizon_refuse_aliasing(const char* what, uint32_t dst,
                                              uint32_t src, uint32_t size)
{
    vwine_logf("horizon: %s(dst=0x%08x, src=0x%08x, size=0x%x) needs the same "
               "physical memory visible at two virtual addresses. MVII runs a "
               "flat 1:1 identity map with no per-process page tables "
               "(mt6592_mmu.c), so there is no second mapping to make. Copying "
               "instead would break the aliasing the guest is relying on -- "
               "writes through one address would not appear at the other.\n",
               what, dst, src, size);
    return HZN_RESULT_INVALID_MEMORY_REGION;
}

horizon_result hzn_map_memory(uint32_t dst, uint32_t src, uint32_t size)
{
    return horizon_refuse_aliasing("svcMapMemory", dst, src, size);
}

horizon_result hzn_unmap_memory(uint32_t dst, uint32_t src, uint32_t size)
{
    return horizon_refuse_aliasing("svcUnmapMemory", dst, src, size);
}

horizon_result hzn_map_shared_memory(uint32_t handle, uint32_t addr, uint32_t size,
                                     uint32_t perm)
{
    (void)handle; (void)perm;
    return horizon_refuse_aliasing("svcMapSharedMemory", addr, 0, size);
}

horizon_result hzn_unmap_shared_memory(uint32_t handle, uint32_t addr, uint32_t size)
{
    (void)handle;
    return horizon_refuse_aliasing("svcUnmapSharedMemory", addr, 0, size);
}

horizon_result hzn_create_transfer_memory(uint32_t addr, uint32_t size, uint32_t perm,
                                          uint32_t* out_handle)
{
    if ((addr & (VWINE_PAGE_SIZE - 1u)) != 0) return HZN_RESULT_INVALID_ADDRESS;
    if (size == 0 || (size & (VWINE_PAGE_SIZE - 1u)) != 0) return HZN_RESULT_INVALID_SIZE;
    if (!horizon_addr_valid(addr, size)) return HZN_RESULT_INVALID_CURRENT_MEMORY;
    (void)perm;

    // A transfer-memory object names a range of the creator's own memory for
    // another process to map. There is only one process here, so the object is
    // exactly the range -- no aliasing needed, and nothing to enforce, because
    // the receiving side would be code in this same address space.
    const uint32_t handle = horizon_handle_create(HZN_OBJECT_TRANSFER_MEMORY,
                                                  (void*)(uintptr_t)addr);
    if (handle == HZN_HANDLE_INVALID) return HZN_RESULT_OUT_OF_HANDLES;
    *out_handle = handle;
    return HZN_RESULT_SUCCESS;
}

horizon_result hzn_query_memory(uint32_t addr, hzn_memory_info* out_info,
                                uint32_t* out_page_info)
{
    memset(out_info, 0, sizeof(*out_info));

    // A region containing the address answers directly.
    for (size_t i = 0; i < HZN_MAX_REGIONS; ++i) {
        if (!g_regions[i].used) continue;
        if ((uintptr_t)addr < g_regions[i].base) continue;
        if ((uintptr_t)addr >= g_regions[i].base + g_regions[i].size) continue;
        out_info->addr = g_regions[i].base;
        out_info->size = g_regions[i].size;
        out_info->state = (uint32_t)g_regions[i].state;
        out_info->attr = g_regions[i].attr;
        out_info->perm = (uint32_t)g_regions[i].perm;
        *out_page_info = 0;
        return HZN_RESULT_SUCCESS;
    }

    // Otherwise report the free gap the address falls in, bounded by whichever
    // regions sit either side of it. Guests walk the address space with this,
    // so a gap of the wrong size sends them round the loop forever.
    //
    // Computed in 64-bit and clamped at the top of the 32-bit space: the guest
    // sees these as 32-bit fields, and a gap that runs to UINTPTR_MAX would
    // reach it as a size one byte short of wrapping.
    const uint64_t space_end = 0x100000000ull;
    uint64_t gap_start = 0;
    uint64_t gap_end = space_end;
    for (size_t i = 0; i < HZN_MAX_REGIONS; ++i) {
        if (!g_regions[i].used) continue;
        const uint64_t end = (uint64_t)g_regions[i].base + g_regions[i].size;
        if (end <= (uint64_t)addr && end > gap_start) gap_start = end;
        if ((uint64_t)g_regions[i].base > (uint64_t)addr &&
            (uint64_t)g_regions[i].base < gap_end)
            gap_end = g_regions[i].base;
    }
    out_info->addr = gap_start;
    out_info->size = gap_end - gap_start;
    out_info->state = HZN_MEMORY_STATE_FREE;
    out_info->perm = HZN_PERM_NONE;
    *out_page_info = 0;
    return HZN_RESULT_SUCCESS;
}

// ── synchronisation ────────────────────────────────────────────────────────

horizon_result hzn_close_handle(uint32_t handle)
{
    if (handle == HZN_HANDLE_CURRENT_THREAD || handle == HZN_HANDLE_CURRENT_PROCESS)
        return HZN_RESULT_SUCCESS;
    if (handle == HZN_HANDLE_INVALID || handle > HZN_MAX_HANDLES)
        return HZN_RESULT_INVALID_HANDLE;

    hzn_handle_entry* e = &g_handles[handle - 1];
    if (e->kind == HZN_OBJECT_NONE) return HZN_RESULT_INVALID_HANDLE;
    if (e->kind == HZN_OBJECT_THREAD) {
        hzn_thread* t = (hzn_thread*)e->payload;
        if (t && t->state == HZN_THREAD_FINISHED) t->used = false;
    }
    if (e->kind == HZN_OBJECT_EVENT) {
        hzn_event* ev = (hzn_event*)e->payload;
        if (ev) ev->used = false;
    }
    if (e->kind == HZN_OBJECT_SESSION) horizon_ipc_release_session(e->payload);
    e->kind = HZN_OBJECT_NONE;
    e->payload = NULL;
    return HZN_RESULT_SUCCESS;
}

static bool horizon_object_signalled(uint32_t handle, bool consume)
{
    if (handle == HZN_HANDLE_INVALID || handle > HZN_MAX_HANDLES) return false;
    const hzn_handle_entry* e = &g_handles[handle - 1];
    switch (e->kind) {
    case HZN_OBJECT_THREAD: {
        const hzn_thread* t = (const hzn_thread*)e->payload;
        return t && t->state == HZN_THREAD_FINISHED;
    }
    case HZN_OBJECT_EVENT: {
        hzn_event* ev = (hzn_event*)e->payload;
        if (!ev || !ev->signalled) return false;
        if (consume && ev->auto_clear) ev->signalled = 0;
        return true;
    }
    default:
        return false;
    }
}

horizon_result hzn_wait_synchronization(const uint32_t* handles, int32_t count,
                                        int64_t timeout_ns, int32_t* out_index)
{
    if (count < 0 || count > 0x40) return HZN_RESULT_OUT_OF_RANGE;
    if (count > 0 && !handles) return HZN_RESULT_INVALID_POINTER;

    for (int32_t i = 0; i < count; ++i) {
        const uint32_t h = handles[i];
        if (h == HZN_HANDLE_INVALID || h > HZN_MAX_HANDLES ||
            g_handles[h - 1].kind == HZN_OBJECT_NONE)
            return HZN_RESULT_INVALID_HANDLE;
        // A session is only waitable when IPC is asynchronous, and every
        // service in this front-end answers on the calling thread, so a wait on
        // one would never be satisfied. Say so instead of hanging.
        if (g_handles[h - 1].kind == HZN_OBJECT_SESSION) {
            vwine_logf("horizon: svcWaitSynchronization on session handle 0x%x. "
                       "Sessions here are answered synchronously inside "
                       "svcSendSyncRequest, so there is no pending-reply state "
                       "to wait for.\n", h);
            return HZN_RESULT_NO_SYNCHRONIZATION_OBJECT;
        }
    }

    const bool forever = (timeout_ns < 0);
    const uint64_t deadline_us = forever ? 0 : horizon_now_us() + (uint64_t)(timeout_ns / 1000);

    for (;;) {
        for (int32_t i = 0; i < count; ++i) {
            if (horizon_object_signalled(handles[i], true)) {
                *out_index = i;
                return HZN_RESULT_SUCCESS;
            }
        }
        if (g_exit_requested) return HZN_RESULT_TERMINATION_REQUESTED;
        if (!forever && horizon_now_us() >= deadline_us) return HZN_RESULT_TIMED_OUT;
        horizon_yield();
    }
}

horizon_result hzn_reset_signal(uint32_t handle)
{
    hzn_event* ev = (hzn_event*)horizon_handle_lookup(handle, HZN_OBJECT_EVENT);
    if (!ev) {
        // Threads are signalled by exiting and cannot be reset; the real kernel
        // says INVALID_STATE for that, not INVALID_HANDLE.
        if (horizon_handle_lookup(handle, HZN_OBJECT_THREAD)) return HZN_RESULT_INVALID_STATE;
        return HZN_RESULT_INVALID_HANDLE;
    }
    if (!ev->signalled) return HZN_RESULT_INVALID_STATE;
    ev->signalled = 0;
    return HZN_RESULT_SUCCESS;
}

horizon_result hzn_clear_event(uint32_t handle)
{
    hzn_event* ev = (hzn_event*)horizon_handle_lookup(handle, HZN_OBJECT_EVENT);
    if (!ev) return HZN_RESULT_INVALID_HANDLE;
    ev->signalled = 0;
    return HZN_RESULT_SUCCESS;
}

uint32_t horizon_event_create(bool auto_clear)
{
    for (size_t i = 0; i < HZN_MAX_EVENTS; ++i) {
        if (g_events[i].used) continue;
        g_events[i].used = true;
        g_events[i].signalled = 0;
        g_events[i].auto_clear = auto_clear;
        const uint32_t handle = horizon_handle_create(HZN_OBJECT_EVENT, &g_events[i]);
        if (handle == HZN_HANDLE_INVALID) g_events[i].used = false;
        return handle;
    }
    vwine_logf("horizon: out of event objects (%u)\n", (unsigned)HZN_MAX_EVENTS);
    return HZN_HANDLE_INVALID;
}

void horizon_event_signal(uint32_t handle)
{
    hzn_event* ev = (hzn_event*)horizon_handle_lookup(handle, HZN_OBJECT_EVENT);
    if (ev) ev->signalled = 1;
}

// ── guest mutexes and condition variables ──────────────────────────────────
//
// These are the interesting ones, because the state lives in GUEST memory and
// the kernel only arbitrates. libnx's mutex is one word: the owning thread's
// handle, with bit 30 meaning "someone is waiting, so the unlock must enter the
// kernel". The guest CASes it itself and only calls svcArbitrateLock when that
// fails.
//
// The read-modify-writes below need no atomics, and that is a property of the
// scheduler rather than luck: MVII never preempts, so a sequence with no yield
// in it cannot be interleaved with another guest thread. Add a yield in the
// middle of one of these and it becomes racy.

#define HZN_MUTEX_WAITERS_BIT 0x40000000u

horizon_result hzn_arbitrate_lock(uint32_t owner_handle, uint32_t tag_addr,
                                  uint32_t requester_handle)
{
    if ((tag_addr & 3u) != 0) return HZN_RESULT_INVALID_ADDRESS;
    if (!horizon_addr_valid(tag_addr, sizeof(uint32_t))) return HZN_RESULT_INVALID_ADDRESS;

    volatile uint32_t* tag = (volatile uint32_t*)(uintptr_t)tag_addr;

    for (;;) {
        const uint32_t value = *tag;
        if (value == 0) {
            *tag = requester_handle;
            return HZN_RESULT_SUCCESS;
        }
        // The owner changed out from under the guest's failed CAS: the lock is
        // no longer held by the thread it named, so this is not the wait it
        // asked for. The real kernel returns success and lets the guest retry.
        if ((value & ~HZN_MUTEX_WAITERS_BIT) != owner_handle)
            return HZN_RESULT_SUCCESS;

        // Tell the owner its unlock has to go through the kernel.
        *tag = value | HZN_MUTEX_WAITERS_BIT;

        if (g_exit_requested) return HZN_RESULT_TERMINATION_REQUESTED;
        horizon_yield();
    }
}

horizon_result hzn_arbitrate_unlock(uint32_t tag_addr)
{
    if ((tag_addr & 3u) != 0) return HZN_RESULT_INVALID_ADDRESS;
    if (!horizon_addr_valid(tag_addr, sizeof(uint32_t))) return HZN_RESULT_INVALID_ADDRESS;

    // Released to whichever waiter runs next rather than handed to the
    // highest-priority one. MVII has no priorities to order them by, so
    // pretending to would just be picking arbitrarily and calling it a policy.
    *(volatile uint32_t*)(uintptr_t)tag_addr = 0;
    return HZN_RESULT_SUCCESS;
}

static hzn_key* horizon_key_for(uint32_t addr, bool create)
{
    for (size_t i = 0; i < HZN_MAX_KEYS; ++i)
        if (g_keys[i].used && g_keys[i].addr == addr) return &g_keys[i];
    if (!create) return NULL;
    for (size_t i = 0; i < HZN_MAX_KEYS; ++i) {
        if (g_keys[i].used) continue;
        g_keys[i].used = true;
        g_keys[i].addr = addr;
        g_keys[i].waiters = 0;
        g_keys[i].to_wake = 0;
        return &g_keys[i];
    }
    vwine_logf("horizon: out of process-wide keys (%u); the guest is waiting on "
               "more distinct condition variables at once than this table holds\n",
               (unsigned)HZN_MAX_KEYS);
    return NULL;
}

horizon_result hzn_wait_process_wide_key_atomic(uint32_t tag_addr, uint32_t key_addr,
                                                uint32_t self_handle, int64_t timeout_ns)
{
    if ((tag_addr & 3u) != 0 || (key_addr & 3u) != 0) return HZN_RESULT_INVALID_ADDRESS;
    if (!horizon_addr_valid(tag_addr, sizeof(uint32_t)) ||
        !horizon_addr_valid(key_addr, sizeof(uint32_t)))
        return HZN_RESULT_INVALID_ADDRESS;

    hzn_key* key = horizon_key_for(key_addr, true);
    if (!key) return HZN_RESULT_OUT_OF_RESOURCE;

    // "Atomic" is the whole point of the call: the mutex must be released and
    // the wait entered with no window in between, or a signal sent in that
    // window is lost. Registering as a waiter before dropping the lock, with no
    // yield between the two, is what closes it here.
    key->waiters++;
    *(volatile uint32_t*)(uintptr_t)tag_addr = 0;

    const bool forever = (timeout_ns < 0);
    const uint64_t deadline_us = forever ? 0 : horizon_now_us() + (uint64_t)(timeout_ns / 1000);
    horizon_result rc = HZN_RESULT_SUCCESS;

    for (;;) {
        if (key->to_wake > 0) {
            key->to_wake--;
            break;
        }
        if (g_exit_requested) { rc = HZN_RESULT_TERMINATION_REQUESTED; break; }
        if (!forever && horizon_now_us() >= deadline_us) { rc = HZN_RESULT_TIMED_OUT; break; }
        horizon_yield();
    }
    key->waiters--;

    // Reacquire on the way out, timeout or not: the guest's condvarWait returns
    // holding the mutex in every case, and its caller unlocks unconditionally.
    const horizon_result lock_rc = hzn_arbitrate_lock(self_handle, tag_addr, self_handle);
    return (rc != HZN_RESULT_SUCCESS) ? rc : lock_rc;
}

horizon_result hzn_signal_process_wide_key(uint32_t key_addr, int32_t count)
{
    if ((key_addr & 3u) != 0) return HZN_RESULT_INVALID_ADDRESS;
    hzn_key* key = horizon_key_for(key_addr, false);
    // Signalling a key nobody waits on is normal and does nothing: Horizon's
    // condition variables have no memory, exactly like pthread's.
    if (!key) return HZN_RESULT_SUCCESS;

    const int32_t wake = (count <= 0) ? (int32_t)key->waiters : count;
    key->to_wake += wake;
    if (key->to_wake > (int32_t)key->waiters) key->to_wake = (int32_t)key->waiters;
    return HZN_RESULT_SUCCESS;
}

// ── time, debug, info ──────────────────────────────────────────────────────

uint64_t hzn_get_system_tick(void)
{
    return (horizon_now_us() * HZN_TICK_HZ) / 1000000ull;
}

uint64_t hzn_get_system_tick_frequency(void) { return HZN_TICK_HZ; }

horizon_result hzn_output_debug_string(uint32_t addr, uint32_t length)
{
    if (length == 0) return HZN_RESULT_SUCCESS;
    if (!horizon_addr_valid(addr, length)) return HZN_RESULT_INVALID_ADDRESS;
    // Precision, not a NUL terminator: svcOutputDebugString takes a counted
    // string and the guest is under no obligation to terminate it.
    if (length > 512u) length = 512u;
    vwine_logf("horizon guest: %.*s\n", (int)length, (const char*)(uintptr_t)addr);
    return HZN_RESULT_SUCCESS;
}

horizon_result hzn_break(uint32_t reason, uint32_t addr, uint32_t size)
{
    // Bit 31 clear means the guest wants to be stopped; set means it is only
    // reporting and expects to continue.
    const bool fatal = (reason & 0x80000000u) == 0;
    vwine_logf("horizon: guest called svcBreak(reason=0x%08x, addr=0x%08x, "
               "size=0x%x)%s\n", reason, addr, size,
               fatal ? " -- this is a fatal break and the guest expects to stop"
                     : " -- notification only, continuing");
    if (!fatal) return HZN_RESULT_SUCCESS;

    g_exit_requested = true;
    g_exit_code = 1;
    if (g_abi && g_abi->exit_fn) g_abi->exit_fn(1);
    return HZN_RESULT_SUCCESS;
}

horizon_result hzn_get_info(uint32_t info_type, uint32_t handle, uint64_t sub_type,
                            uint64_t* out_value)
{
    enum {
        ALLOWED_CPU_CORE_MASK = 0,
        ALLOWED_THREAD_PRIORITY_MASK = 1,
        MAP_REGION_BASE_ADDR = 2,
        MAP_REGION_SIZE = 3,
        HEAP_REGION_BASE_ADDR = 4,
        HEAP_REGION_SIZE = 5,
        TOTAL_PHYSICAL_MEMORY_AVAILABLE = 6,
        TOTAL_PHYSICAL_MEMORY_USED = 7,
        IS_CURRENT_PROCESS_BEING_DEBUGGED = 8,
        RANDOM_ENTROPY = 11,
        ASLR_REGION_BASE_ADDR = 12,
        ASLR_REGION_SIZE = 13,
        STACK_REGION_BASE_ADDR = 14,
        STACK_REGION_SIZE = 15,
        SYSTEM_RESOURCE_SIZE = 16,
        SYSTEM_RESOURCE_USAGE = 17,
        TITLE_ID = 18,
        USER_EXCEPTION_CONTEXT_ADDR = 20,
        TOTAL_PHYSICAL_MEMORY_AVAILABLE_WITHOUT_SYSTEM_RESOURCE = 21,
        TOTAL_PHYSICAL_MEMORY_USED_WITHOUT_SYSTEM_RESOURCE = 22,
        THREAD_TICK_COUNT = 0xF0000002,
    };

    if (handle != HZN_HANDLE_CURRENT_PROCESS && handle != HZN_HANDLE_CURRENT_THREAD &&
        handle != HZN_HANDLE_INVALID && !horizon_thread_from_handle(handle))
        return HZN_RESULT_INVALID_HANDLE;

    switch (info_type) {
    case ALLOWED_CPU_CORE_MASK:
        *out_value = 1;   // one core, and it is core 0
        return HZN_RESULT_SUCCESS;
    case ALLOWED_THREAD_PRIORITY_MASK:
        *out_value = 0xFFFFFFFFFFFFFFFFull;   // all 64 priorities accepted
        return HZN_RESULT_SUCCESS;
    case HEAP_REGION_BASE_ADDR:
        *out_value = (uint64_t)(uintptr_t)g_heap.base;
        return HZN_RESULT_SUCCESS;
    case HEAP_REGION_SIZE:
        *out_value = g_heap.length;
        return HZN_RESULT_SUCCESS;
    case TOTAL_PHYSICAL_MEMORY_AVAILABLE:
    case TOTAL_PHYSICAL_MEMORY_AVAILABLE_WITHOUT_SYSTEM_RESOURCE:
        // What the guest may actually get, which is the heap reservation --
        // not a round number chosen to look generous. A guest that sizes its
        // allocator off this and then cannot allocate is worse off than one
        // told the truth up front.
        *out_value = g_heap.length;
        return HZN_RESULT_SUCCESS;
    case TOTAL_PHYSICAL_MEMORY_USED:
    case TOTAL_PHYSICAL_MEMORY_USED_WITHOUT_SYSTEM_RESOURCE:
        *out_value = g_heap_committed + g_code_size;
        return HZN_RESULT_SUCCESS;
    case IS_CURRENT_PROCESS_BEING_DEBUGGED:
        *out_value = 0;
        return HZN_RESULT_SUCCESS;
    case STACK_REGION_BASE_ADDR:
        *out_value = g_stack_base;
        return HZN_RESULT_SUCCESS;
    case STACK_REGION_SIZE:
        *out_value = g_stack_size;
        return HZN_RESULT_SUCCESS;
    case ASLR_REGION_BASE_ADDR:
        *out_value = g_code_base;
        return HZN_RESULT_SUCCESS;
    case ASLR_REGION_SIZE:
        *out_value = g_code_size;
        return HZN_RESULT_SUCCESS;
    case SYSTEM_RESOURCE_SIZE:
    case SYSTEM_RESOURCE_USAGE:
        *out_value = 0;
        return HZN_RESULT_SUCCESS;
    case THREAD_TICK_COUNT:
        // Per-thread CPU time. MVII does not account it, and a guest that
        // profiles with this would read a wall clock and call it CPU time.
        vwine_logf("horizon: svcGetInfo(THREAD_TICK_COUNT) -- MVII does not "
                   "account per-thread CPU time, and returning the wall clock "
                   "would silently answer a different question\n");
        return HZN_RESULT_INVALID_ENUM_VALUE;
    default:
        vwine_logf("horizon: svcGetInfo(id=%u, sub=%llu, handle=0x%08x) is not "
                   "implemented. Every value this returns is one the guest will "
                   "compute with, so it is not stubbed.\n",
                   info_type, (unsigned long long)sub_type, handle);
        return HZN_RESULT_INVALID_ENUM_VALUE;
    }
}
