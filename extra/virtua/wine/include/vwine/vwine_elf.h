// vwine_elf.h — mapping and relocating a guest ARMv7 ELF32 image.
//
// This is the step that makes the whole approach work, and the reason it is
// only a few hundred lines: the guest's instructions are ALREADY the host's
// instructions. Vita is ARMv7-A Cortex-A9, AArch32 Horizon is ARMv7-A, and MVII
// runs on an ARMv7-A Cortex-A7. There is no decode, no dispatch, no translation
// and no recompilation -- the loader places the bytes at an address, fixes the
// addresses inside them, points the imports at host implementations, and
// branches. That is the entire CPU story.
//
// What is left is ordinary dynamic-linker work, and it is done here rather than
// borrowed because MVII has no dynamic linker to borrow from: guests are static
// PIE images (see the -static -fpie link in VirtuaArmApp.cmake), so nothing in
// the platform knows how to process a DT_REL table.
//
// ── ELF types are defined locally, on purpose ──────────────────────────────
//
// The bare-metal ARM sysroot PowerEngine builds is llvm-libc plus libc++; it
// has no <elf.h>, because nothing on the device loads an ELF at runtime. These
// structures are fixed by the ELF32 and ARM ELF ABI specifications and have not
// changed in twenty years, so declaring them is a dozen lines and adds no
// dependency.
//
// ── What is deliberately not here ──────────────────────────────────────────
//
// No AArch64. A Cortex-A7 cannot execute 64-bit ARM instructions at all, so a
// 64-bit guest is not a "harder case", it is a different project requiring a
// full emulator. vwine_elf_identify reports it as unsupported and names the
// reason, which is the honest answer and the one that stops someone debugging a
// load failure that was never going to load.

#ifndef VWINE_ELF_H
#define VWINE_ELF_H

#include <stddef.h>
#include <stdint.h>

#include "vwine/vwine_mem.h"
#include "vwine/vwine_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── ELF32 ──────────────────────────────────────────────────────────────────

typedef uint32_t Vwine_Elf32_Addr;
typedef uint32_t Vwine_Elf32_Off;
typedef uint32_t Vwine_Elf32_Word;
typedef int32_t  Vwine_Elf32_Sword;
typedef uint16_t Vwine_Elf32_Half;

#define VWINE_EI_NIDENT 16

typedef struct {
    unsigned char    e_ident[VWINE_EI_NIDENT];
    Vwine_Elf32_Half e_type;
    Vwine_Elf32_Half e_machine;
    Vwine_Elf32_Word e_version;
    Vwine_Elf32_Addr e_entry;
    Vwine_Elf32_Off  e_phoff;
    Vwine_Elf32_Off  e_shoff;
    Vwine_Elf32_Word e_flags;
    Vwine_Elf32_Half e_ehsize;
    Vwine_Elf32_Half e_phentsize;
    Vwine_Elf32_Half e_phnum;
    Vwine_Elf32_Half e_shentsize;
    Vwine_Elf32_Half e_shnum;
    Vwine_Elf32_Half e_shstrndx;
} Vwine_Elf32_Ehdr;

typedef struct {
    Vwine_Elf32_Word p_type;
    Vwine_Elf32_Off  p_offset;
    Vwine_Elf32_Addr p_vaddr;
    Vwine_Elf32_Addr p_paddr;
    Vwine_Elf32_Word p_filesz;
    Vwine_Elf32_Word p_memsz;
    Vwine_Elf32_Word p_flags;
    Vwine_Elf32_Word p_align;
} Vwine_Elf32_Phdr;

typedef struct {
    Vwine_Elf32_Sword d_tag;
    union {
        Vwine_Elf32_Word d_val;
        Vwine_Elf32_Addr d_ptr;
    } d_un;
} Vwine_Elf32_Dyn;

typedef struct {
    Vwine_Elf32_Addr r_offset;
    Vwine_Elf32_Word r_info;
} Vwine_Elf32_Rel;

typedef struct {
    Vwine_Elf32_Word st_name;
    Vwine_Elf32_Addr st_value;
    Vwine_Elf32_Word st_size;
    unsigned char    st_info;
    unsigned char    st_other;
    Vwine_Elf32_Half st_shndx;
} Vwine_Elf32_Sym;

#define VWINE_ELF32_R_SYM(i)  ((i) >> 8)
#define VWINE_ELF32_R_TYPE(i) ((unsigned char)(i))

// e_type
#define VWINE_ET_EXEC 2
#define VWINE_ET_DYN  3

// e_machine
#define VWINE_EM_ARM     40
#define VWINE_EM_AARCH64 183

// p_type
#define VWINE_PT_LOAD    1
#define VWINE_PT_DYNAMIC 2

// p_flags
#define VWINE_PF_X 0x1
#define VWINE_PF_W 0x2
#define VWINE_PF_R 0x4

// d_tag
#define VWINE_DT_NULL          0
#define VWINE_DT_STRTAB        5
#define VWINE_DT_SYMTAB        6
#define VWINE_DT_STRSZ        10
#define VWINE_DT_SYMENT       11
#define VWINE_DT_INIT         12
#define VWINE_DT_FINI         13
#define VWINE_DT_REL          17
#define VWINE_DT_RELSZ        18
#define VWINE_DT_RELENT       19
#define VWINE_DT_JMPREL       23
#define VWINE_DT_PLTRELSZ      2
#define VWINE_DT_INIT_ARRAY   25
#define VWINE_DT_FINI_ARRAY   26
#define VWINE_DT_INIT_ARRAYSZ 27
#define VWINE_DT_FINI_ARRAYSZ 28

// ARM relocation types (ARM ELF ABI, "Relocation types" table).
#define VWINE_R_ARM_NONE       0
#define VWINE_R_ARM_ABS32      2
#define VWINE_R_ARM_REL32      3
#define VWINE_R_ARM_GLOB_DAT  21
#define VWINE_R_ARM_JUMP_SLOT 22
#define VWINE_R_ARM_RELATIVE  23

// ── what a load produces ───────────────────────────────────────────────────

typedef struct vwine_image {
    vwine_mapping mapping;    // the single contiguous allocation
    uintptr_t     load_bias;  // add to a link-time vaddr to get a real address
    uintptr_t     entry;      // real address of the guest entry point
    uintptr_t     span_base;  // lowest link-time vaddr in the image
    size_t        span;       // bytes from span_base to the end of the image

    const Vwine_Elf32_Dyn* dynamic;      // in the mapped image, or NULL
    const char*            name;         // for diagnostics; not owned

    // Resolved from PT_DYNAMIC. Real addresses, already biased.
    const Vwine_Elf32_Sym* symtab;
    const char*            strtab;
    size_t                 strtab_size;
    size_t                 syment;

    uintptr_t init_array;
    size_t    init_array_count;
    uintptr_t fini_array;
    size_t    fini_array_count;
    uintptr_t init_func;
    uintptr_t fini_func;
} vwine_image;

// What kind of guest a blob is, decided before anything is mapped so the
// refusal can be specific.
typedef enum vwine_elf_kind {
    VWINE_ELF_INVALID = 0,       // not an ELF, or truncated
    VWINE_ELF_ARM32,             // loadable here
    VWINE_ELF_AARCH64,           // 64-bit: cannot execute on Cortex-A7
    VWINE_ELF_FOREIGN_MACHINE,   // some other architecture entirely
} vwine_elf_kind;

// Classify `data` without mapping it. `size` must be the whole blob.
vwine_elf_kind vwine_elf_identify(const void* data, size_t size);

// Human-readable form of the above, for refusal messages.
const char* vwine_elf_kind_text(vwine_elf_kind kind);

// Map every PT_LOAD of an ARM ELF32 into one contiguous allocation and record
// the dynamic tables. Does NOT relocate and does NOT make the image executable;
// callers run vwine_elf_relocate and then vwine_image_finalize.
//
// Returns 0 on success, negative errno otherwise, with the reason logged.
int vwine_elf_load(const void* data, size_t size, const char* name,
                   vwine_image* out);

// Read `image->dynamic` and fill in the tables it names: symtab, strtab, the
// init/fini functions and arrays. `dynamic` and `load_bias` must already be set;
// everything this touches is overwritten.
//
// vwine_elf_load calls it for you. It is public for the loader that does NOT go
// through vwine_elf_load: a Horizon NRO/NSO is not an ELF file and has no
// program headers, so its front-end places the segments itself and then points
// `dynamic` at the MOD0 header's .dynamic. The DT_* walk after that is
// identical, and identical code in two places is the bug where one copy gets
// fixed.
void vwine_image_scan_dynamic(vwine_image* image);

// Apply DT_REL and DT_JMPREL.
//
// Symbol-bearing relocations resolve through `registry`; every miss is recorded
// in `missing` rather than aborting, so one pass reports every unimplemented
// import. The relocation itself is then left pointing at nothing, which is safe
// only because the caller is required to refuse the run when `missing` is
// non-empty -- see the note in vwine_registry.h.
//
// `registry` and `missing` may both be NULL for an image with no imports.
//
// Returns 0 when every relocation was of a supported type, negative errno when
// an unsupported relocation type was found (which is a loader bug or a guest
// built with an unexpected toolchain, and is not survivable).
int vwine_elf_relocate(vwine_image* image, const vwine_registry* registry,
                       vwine_missing_set* missing);

// Make the whole image safe to execute. Call after the last write to it.
int vwine_image_finalize(vwine_image* image);

// Run DT_INIT and DT_INIT_ARRAY, in that order, as the ARM ABI specifies.
// Must be called after vwine_image_finalize -- these are guest functions, so
// branching to them before the I-cache is coherent is the exact bug
// vwine_make_executable exists to prevent.
void vwine_image_run_initializers(const vwine_image* image);

// Run DT_FINI_ARRAY (in reverse) and DT_FINI.
void vwine_image_run_finalizers(const vwine_image* image);

void vwine_image_release(vwine_image* image);

#ifdef __cplusplus
}
#endif

#endif  // VWINE_ELF_H
