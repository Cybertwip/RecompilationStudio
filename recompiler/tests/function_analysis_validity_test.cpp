#include "function_analysis.h"

#include <cstdint>
#include <iostream>

namespace {

struct TestCase {
    const char* name;
    std::uint32_t word;
    bool expected;
};

} // namespace

int main() {
    const TestCase cases[] = {
        {"canonical jr ra", 0x03E00008u, true},
        {"Sonic Wings packed data masquerading as jr", 0x00016A08u, false},
        {"canonical jalr t9", 0x0320F809u, true},
        {"reserved SPECIAL funct", 0x00203A41u, false},
        {"canonical nop", 0x00000000u, true},
        {"R3000 GTE RTPS", 0x4A180001u, true},
        {"COP0 RFE", 0x42000010u, true},
    };

    int failures = 0;
    for (const auto& test : cases) {
        const bool actual =
            PSXRecomp::FunctionAnalyzer::is_valid_mips_word(test.word);
        if (actual != test.expected) {
            std::cerr << "FAIL: " << test.name << " (0x" << std::hex
                      << test.word << std::dec << "): expected "
                      << (test.expected ? "valid" : "invalid") << ", got "
                      << (actual ? "valid" : "invalid") << '\n';
            ++failures;
        }
    }

    if (failures == 0) {
        std::cout << "Function-analysis instruction validity regression passed\n";
    }
    return failures == 0 ? 0 : 1;
}
