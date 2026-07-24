#include "cpu_state.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

int g_psx_call_bail = 0;
uint64_t g_psx_bail_first = 0;
uint64_t g_psx_bail_resolved = 0;
uint64_t g_psx_bail_flattened = 0;
uint64_t g_psx_bail_anomaly = 0;

void psx_bail_record(uint32_t site_ra, uint32_t site_sp,
                     uint32_t wild_pc, uint32_t guest_sp) {
    (void)site_ra; (void)site_sp; (void)wild_pc; (void)guest_sp;
}

/* Mirrors the corrected interpreter/JIT call-result ordering. */
static int finish_direct_call(CPUState *cpu, uint32_t site_ra, uint32_t site_sp) {
    if (g_psx_call_bail) return 1;
    if (psx_call_result_is_transfer(cpu, site_ra)) return 1;
    return psx_call_contract(cpu, site_ra, site_sp);
}

int main(void) {
    const uint32_t site_ra = 0x801D2CB8u;
    const uint32_t site_sp = 0x8006AC08u;
    CPUState cpu;
    memset(&cpu, 0, sizeof(cpu));

    /* Direct compiled callees return with pc=$ra. Consume it without touching v0. */
    cpu.pc = site_ra;
    cpu.gpr[31] = site_ra;
    cpu.gpr[29] = site_sp;
    cpu.gpr[2] = 0x1482060Au;
    assert(finish_direct_call(&cpu, site_ra, site_sp) == 0);
    assert(cpu.pc == 0u);
    assert(cpu.gpr[2] == 0x1482060Au);

    /* A real nonlocal transfer has precedence over the call contract. Exception
     * paths legitimately surface with a different PC and mismatched RA/SP; this
     * must not fabricate a bail (Chrono Cross startup regression at 0xE28). */
    cpu.pc = 0x800164F4u;
    cpu.gpr[31] = 0x80015D34u;
    cpu.gpr[29] = 0x00008568u;
    assert(finish_direct_call(&cpu, site_ra, site_sp) == 1);
    assert(cpu.pc == 0x800164F4u);
    assert(g_psx_call_bail == 0);
    assert(g_psx_bail_first == 0u);

    /* With no surfaced PC, a real RA/SP contract violation still begins bail. */
    cpu.pc = 0u;
    cpu.gpr[31] = 0x800164F4u;
    cpu.gpr[29] = site_sp;
    assert(finish_direct_call(&cpu, site_ra, site_sp) == 1);
    assert(g_psx_call_bail == 1);
    assert(g_psx_bail_first == 1u);
    return 0;
}
