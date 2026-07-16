#include "function_analysis.h"

#include <cstdint>
#include <cstring>
#include <iostream>

namespace {

struct TestCase {
    const char* name;
    std::uint32_t word;
    bool expected;
};

} // namespace

static void write_word(PSXRecomp::PS1Executable& exe,
                       std::uint32_t offset,
                       std::uint32_t word) {
    exe.code_data[offset + 0] = static_cast<std::uint8_t>(word);
    exe.code_data[offset + 1] = static_cast<std::uint8_t>(word >> 8);
    exe.code_data[offset + 2] = static_cast<std::uint8_t>(word >> 16);
    exe.code_data[offset + 3] = static_cast<std::uint8_t>(word >> 24);
}

static bool has_code_extent(const PSXRecomp::FunctionAnalysisResult& analysis,
                            std::uint32_t entry,
                            std::uint32_t end) {
    for (const auto& function : analysis.functions) {
        if (function.start_addr == entry) {
            return !function.is_data_section && function.end_addr == end;
        }
    }
    return false;
}

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

    // Sonic Wings' real entry ends in BREAK, followed immediately by packed
    // data containing the malformed JR word above. BREAK transfers control to
    // the exception vector and has no fallthrough; the function boundary must
    // stop before the packed data.
    constexpr std::uint32_t load = 0x80060318u;
    PSXRecomp::PS1Executable exe{};
    std::memcpy(exe.header.magic, "PS-X EXE", 8);
    exe.header.initial_pc = load;
    exe.header.load_address = load;
    exe.header.file_size = 0x20u;
    exe.code_data.assign(exe.header.file_size, 0xFFu);
    write_word(exe, 0x00u, 0x3C02800Eu); // lui v0,0x800e
    write_word(exe, 0x04u, 0x0000004Du); // break 0x1
    write_word(exe, 0x08u, 0x00016A08u); // packed data, not JR

    PSXRecomp::FunctionAnalyzer analyzer(exe);
    analyzer.add_forced_entry(load);
    const auto analysis = analyzer.analyze();
    if (!has_code_extent(analysis, load, load + 8u)) {
        std::cerr << "FAIL: BREAK-terminated entry absorbed trailing packed data\n";
        ++failures;
    }

    // A SYSCALL resumes at the following instruction after the exception
    // handler returns. Sonic Wings' ExitCriticalSection wrapper is exactly
    // `li a0,2; syscall; jr ra; nop`; truncating it at SYSCALL leaks the
    // runtime's temporary pc=0 sentinel and terminates the whole game.
    constexpr std::uint32_t syscall_entry = 0x8007B9E4u;
    PSXRecomp::PS1Executable syscall_exe{};
    std::memcpy(syscall_exe.header.magic, "PS-X EXE", 8);
    syscall_exe.header.initial_pc = syscall_entry;
    syscall_exe.header.load_address = syscall_entry;
    syscall_exe.header.file_size = 0x20u;
    syscall_exe.code_data.assign(syscall_exe.header.file_size, 0xFFu);
    write_word(syscall_exe, 0x00u, 0x24040002u); // li a0,2
    write_word(syscall_exe, 0x04u, 0x0000000Cu); // syscall
    write_word(syscall_exe, 0x08u, 0x03E00008u); // jr ra
    write_word(syscall_exe, 0x0Cu, 0x00000000u); // nop (delay slot)
    write_word(syscall_exe, 0x10u, 0x00016A08u); // trailing packed data

    PSXRecomp::FunctionAnalyzer syscall_analyzer(syscall_exe);
    syscall_analyzer.add_forced_entry(syscall_entry);
    const auto syscall_analysis = syscall_analyzer.analyze();
    if (!has_code_extent(syscall_analysis, syscall_entry, syscall_entry + 0x10u)) {
        std::cerr << "FAIL: SYSCALL wrapper lost its post-exception jr ra\n";
        ++failures;
    }

    if (failures == 0) {
        std::cout << "Function-analysis validity and trap-boundary regressions passed\n";
    }
    return failures == 0 ? 0 : 1;
}
