// gba_mvii_dispatch_stub.cpp — an empty cartridge dispatch table.
//
// Linked only when the package carries no recompiled cartridge (see
// GBA_MVII_NATIVE_SOURCES in ../../CMakeLists.txt). gbarecomp's runtime_dispatch
// references these two symbols unconditionally, so something has to define
// them; a game's generated dispatch_table.cpp normally does.
//
// A length of zero is not a placeholder that misbehaves. Every dispatch misses,
// falls through runtime_dispatch_miss and is bridged by the reference
// interpreter, which is a correct GBA — just a slow one, and one the coverage
// report will call NOT STATIC because that is precisely what it is. That
// honesty is the point: a build with no recompiled code should be visibly a
// build with no recompiled code.

#include <cstdint>

struct DispatchEntry {
    uint32_t addr;
    uint8_t  thumb;
    void   (*fn)(void);
};

// One never-matching entry rather than a zero-length array, which is not
// standard C++ and which the table lookup would still be handed a pointer to.
extern "C" const DispatchEntry kDispatchTable[1] = {
    {0xFFFFFFFFu, 0u, nullptr},
};
extern "C" const unsigned kDispatchTableLen = 0u;
