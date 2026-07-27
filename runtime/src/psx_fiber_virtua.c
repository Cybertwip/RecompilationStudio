/* ARMv7 cooperative fiber backend for MVII Virtua applications. */
#include "psx_fiber.h"

#include <stdint.h>
#include <stdlib.h>

#ifndef PSX_VIRTUA_FIBER_STACK_FLOOR
#define PSX_VIRTUA_FIBER_STACK_FLOOR (16u * 1024u)
#endif

typedef struct psx_fiber_context {
    uint32_t r4_r11[8];
    uint32_t sp;
    uint32_t lr;
    uint64_t d8_d15[8];
} psx_fiber_context;

typedef struct psx_fiber_impl {
    psx_fiber_context context;
    void *stack;
    psx_fiber_entry entry;
    void *arg;
} psx_fiber_impl;

extern void psx_virtua_fiber_swap(psx_fiber_context *from,
                                  const psx_fiber_context *to);

static psx_fiber_impl *s_current;

static __attribute__((noreturn)) void psx_virtua_fiber_trampoline(void)
{
    psx_fiber_impl *fiber = s_current;
    if (!fiber || !fiber->entry)
        abort();
    fiber->entry(fiber->arg);
    abort();
}

psx_fiber_t psx_fiber_convert_thread(void)
{
    if (!s_current)
        s_current = (psx_fiber_impl *)calloc(1, sizeof(*s_current));
    return (psx_fiber_t)s_current;
}

psx_fiber_t psx_fiber_current(void)
{
    return (psx_fiber_t)s_current;
}

psx_fiber_t psx_fiber_create(size_t stack_size, psx_fiber_entry entry, void *arg)
{
    if (!entry)
        return NULL;
    if (stack_size < PSX_VIRTUA_FIBER_STACK_FLOOR)
        stack_size = PSX_VIRTUA_FIBER_STACK_FLOOR;
    stack_size = (stack_size + 15u) & ~(size_t)15u;

    psx_fiber_impl *fiber = (psx_fiber_impl *)calloc(1, sizeof(*fiber));
    if (!fiber)
        return NULL;
    fiber->stack = aligned_alloc(16u, stack_size);
    if (!fiber->stack) {
        free(fiber);
        return NULL;
    }
    fiber->entry = entry;
    fiber->arg = arg;
    fiber->context.sp = (uint32_t)(uintptr_t)fiber->stack + (uint32_t)stack_size;
    fiber->context.sp &= ~7u;
    fiber->context.lr = (uint32_t)(uintptr_t)&psx_virtua_fiber_trampoline;
    return (psx_fiber_t)fiber;
}

void psx_fiber_switch(psx_fiber_t target)
{
    psx_fiber_impl *to = (psx_fiber_impl *)target;
    psx_fiber_impl *from = s_current;
    if (!from || !to || from == to)
        return;
    s_current = to;
    psx_virtua_fiber_swap(&from->context, &to->context);
}

void psx_fiber_destroy(psx_fiber_t handle)
{
    psx_fiber_impl *fiber = (psx_fiber_impl *)handle;
    if (!fiber || fiber == s_current)
        return;
    free(fiber->stack);
    free(fiber);
}
