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

int main(void) {
    const uint32_t site_ra = 0x801D2CB8u;
    const uint32_t site_sp = 0x8006AC08u;
    CPUState cpu;
    memset(&cpu, 0, sizeof(cpu));

    /* Direct compiled callees return with pc=$ra. Normalize that to the same
     * pc=0 convention produced by psx_dispatch_call, without touching $v0. */
    cpu.pc = site_ra;
    cpu.gpr[31] = site_ra;
    cpu.gpr[29] = site_sp;
    cpu.gpr[2] = 0x1482060Au;
    assert(psx_call_contract(&cpu, site_ra, site_sp) == 0);
    assert(psx_call_result_is_transfer(&cpu, site_ra) == 0);
    assert(cpu.pc == 0u);
    assert(cpu.gpr[2] == 0x1482060Au);

    /* A real nonlocal transfer remains visible to the caller. */
    cpu.pc = 0x801D3000u;
    cpu.gpr[31] = site_ra;
    cpu.gpr[29] = site_sp;
    assert(psx_call_contract(&cpu, site_ra, site_sp) == 0);
    assert(psx_call_result_is_transfer(&cpu, site_ra) == 1);
    assert(cpu.pc == 0x801D3000u);

    /* Bail resolution still runs before normalization and resolves at the site. */
    g_psx_call_bail = 1;
    cpu.pc = site_ra;
    cpu.gpr[31] = site_ra;
    cpu.gpr[29] = site_sp;
    assert(psx_call_contract(&cpu, site_ra, site_sp) == 0);
    assert(g_psx_call_bail == 0);
    assert(cpu.pc == 0u);
    assert(g_psx_bail_resolved == 1u);
    return 0;
}
