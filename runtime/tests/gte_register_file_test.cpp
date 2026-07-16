#include "gte.h"
#include "cpu_state.h"

#include <cstdlib>
#include <cstdint>
#include <cstring>

extern "C" uint64_t s_frame_count = 0;
extern "C" uint32_t g_debug_current_func_addr = 0;
extern "C" int gpu_ws_present_native_43(void) { return 1; }
extern "C" void psx_ws_note_gte_project(int) {}
extern "C" void psx_gte_set(CPUState*, uint32_t) {}
extern "C" uint32_t psx_gte_cmd_latency(uint32_t) { return 0; }

static void require(bool condition)
{
    if (!condition) std::abort();
}

static uint32_t next_random(uint32_t& state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static void reference_unpack(PSXRecomp::GTE::GTEState& gte,
                             const uint32_t data[32], const uint32_t ctrl[32],
                             bool preserve_flag)
{
    using namespace PSXRecomp::GTE;
    for (int i = 0; i < 32; ++i) {
        if (i == 15 || i == 28) continue;
        gte_mtc2(&gte, static_cast<uint8_t>(i), data[i]);
    }
    for (int i = 0; i < 32; ++i)
        gte_ctc2(&gte, static_cast<uint8_t>(i), ctrl[i]);
    if (preserve_flag) gte.FLAG = ctrl[31];
}

static void compare_pack(const PSXRecomp::GTE::GTEState& gte,
                         const uint32_t data[32], const uint32_t ctrl[32])
{
    using namespace PSXRecomp::GTE;
    uint32_t packed_data[32] = {};
    uint32_t packed_ctrl[32] = {};
    gte_pack_registers(&gte, packed_data, packed_ctrl);
    for (int i = 0; i < 32; ++i) {
        require(packed_data[i] == gte_mfc2(const_cast<GTEState*>(&gte),
                                           static_cast<uint8_t>(i)));
        require(packed_ctrl[i] == gte_cfc2(const_cast<GTEState*>(&gte),
                                           static_cast<uint8_t>(i)));
    }
    (void)data;
    (void)ctrl;
}

int main()
{
    using namespace PSXRecomp::GTE;
    uint32_t random = 0xC001D00Du;
    for (int iteration = 0; iteration < 10000; ++iteration) {
        uint32_t data[32];
        uint32_t ctrl[32];
        for (uint32_t& value : data) value = next_random(random);
        for (uint32_t& value : ctrl) value = next_random(random);

        const bool preserve_modes[2] = {false, true};
        for (bool preserve_flag : preserve_modes) {
            GTEState reference;
            GTEState bulk;
            reference_unpack(reference, data, ctrl, preserve_flag);
            gte_unpack_registers(&bulk, data, ctrl, preserve_flag);
            require(std::memcmp(&reference, &bulk, sizeof(GTEState)) == 0);
            compare_pack(bulk, data, ctrl);
        }
    }
    return 0;
}
