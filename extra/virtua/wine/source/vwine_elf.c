#include "vwine/vwine_elf.h"
#include "vwine/vwine_log.h"

#include <errno.h>
#include <string.h>

// ── identification ─────────────────────────────────────────────────────────

vwine_elf_kind vwine_elf_identify(const void* data, size_t size)
{
    if (!data || size < sizeof(Vwine_Elf32_Ehdr)) return VWINE_ELF_INVALID;
    const unsigned char* id = (const unsigned char*)data;
    if (id[0] != 0x7f || id[1] != 'E' || id[2] != 'L' || id[3] != 'F')
        return VWINE_ELF_INVALID;

    const unsigned char elf_class = id[4];   // EI_CLASS: 1 = 32-bit, 2 = 64-bit
    const unsigned char elf_data = id[5];    // EI_DATA:  1 = little-endian
    if (elf_data != 1) return VWINE_ELF_FOREIGN_MACHINE;

    if (elf_class == 2) {
        // 64-bit. e_machine sits at the same offset in both classes, so this
        // read is valid even though the rest of the header is not our layout.
        const Vwine_Elf32_Ehdr* h = (const Vwine_Elf32_Ehdr*)data;
        if (h->e_machine == VWINE_EM_AARCH64) return VWINE_ELF_AARCH64;
        return VWINE_ELF_FOREIGN_MACHINE;
    }
    if (elf_class != 1) return VWINE_ELF_INVALID;

    const Vwine_Elf32_Ehdr* h = (const Vwine_Elf32_Ehdr*)data;
    if (h->e_machine != VWINE_EM_ARM) return VWINE_ELF_FOREIGN_MACHINE;
    if (h->e_type != VWINE_ET_EXEC && h->e_type != VWINE_ET_DYN)
        return VWINE_ELF_INVALID;
    return VWINE_ELF_ARM32;
}

const char* vwine_elf_kind_text(vwine_elf_kind kind)
{
    switch (kind) {
    case VWINE_ELF_ARM32:   return "ARMv7 (32-bit) — executes natively";
    case VWINE_ELF_AARCH64:
        // Worth spelling out. This is the single most likely reason a Switch
        // title will not start here, and "unsupported" on its own sends people
        // looking for a missing feature rather than a missing CPU.
        return "AArch64 (64-bit) — this device is a 32-bit Cortex-A7 and "
               "cannot execute 64-bit ARM instructions";
    case VWINE_ELF_FOREIGN_MACHINE: return "a non-ARM or big-endian image";
    case VWINE_ELF_INVALID: default: return "not a loadable ELF";
    }
}

// ── loading ────────────────────────────────────────────────────────────────

// Bounds check that survives a hostile or truncated file: both the offset and
// the length are checked against the blob, and the addition cannot wrap.
static int range_ok(size_t size, uint32_t offset, uint32_t length)
{
    if (offset > size) return 0;
    if (length > size) return 0;
    return (size_t)offset + (size_t)length <= size;
}

int vwine_elf_load(const void* data, size_t size, const char* name,
                   vwine_image* out)
{
    if (!data || !out) return -EINVAL;
    memset(out, 0, sizeof(*out));
    out->name = name;

    const vwine_elf_kind kind = vwine_elf_identify(data, size);
    if (kind != VWINE_ELF_ARM32) {
        vwine_logf("vwine: %s is %s\n", name ? name : "image",
                   vwine_elf_kind_text(kind));
        return -ENOEXEC;
    }

    const uint8_t* blob = (const uint8_t*)data;
    const Vwine_Elf32_Ehdr* eh = (const Vwine_Elf32_Ehdr*)data;

    if (eh->e_phentsize != sizeof(Vwine_Elf32_Phdr) || eh->e_phnum == 0) {
        vwine_logf("vwine: %s has an unusable program header table "
                   "(phentsize %u, phnum %u)\n", name ? name : "image",
                   (unsigned)eh->e_phentsize, (unsigned)eh->e_phnum);
        return -ENOEXEC;
    }
    if (!range_ok(size, eh->e_phoff,
                  (uint32_t)eh->e_phnum * (uint32_t)eh->e_phentsize)) {
        vwine_logf("vwine: %s program headers run past the end of the file\n",
                   name ? name : "image");
        return -ENOEXEC;
    }
    const Vwine_Elf32_Phdr* ph = (const Vwine_Elf32_Phdr*)(blob + eh->e_phoff);

    // Pass one: the span every PT_LOAD must fit into. Segments are placed at
    // their link-time addresses relative to the lowest one, which keeps the
    // distances between them exactly as linked -- PC-relative code and the GOT
    // both depend on that, so packing them tightly would break the image.
    uint32_t lowest = 0xFFFFFFFFu;
    uint32_t highest = 0;
    int loads = 0;
    for (unsigned i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != VWINE_PT_LOAD) continue;
        if (ph[i].p_memsz == 0) continue;
        const uint32_t end = ph[i].p_vaddr + ph[i].p_memsz;
        if (end < ph[i].p_vaddr) {
            vwine_logf("vwine: %s segment %u wraps the address space\n",
                       name ? name : "image", i);
            return -ENOEXEC;
        }
        if (ph[i].p_filesz > ph[i].p_memsz ||
            !range_ok(size, ph[i].p_offset, ph[i].p_filesz)) {
            vwine_logf("vwine: %s segment %u is truncated "
                       "(offset %u, filesz %u, memsz %u, file %zu)\n",
                       name ? name : "image", i, (unsigned)ph[i].p_offset,
                       (unsigned)ph[i].p_filesz, (unsigned)ph[i].p_memsz, size);
            return -ENOEXEC;
        }
        if (ph[i].p_vaddr < lowest) lowest = ph[i].p_vaddr;
        if (end > highest) highest = end;
        ++loads;
    }
    if (loads == 0) {
        vwine_logf("vwine: %s has no loadable segments\n", name ? name : "image");
        return -ENOEXEC;
    }

    // Page-align the base of the span so segment page offsets are preserved.
    const uint32_t base_vaddr = lowest & ~(uint32_t)(VWINE_PAGE_SIZE - 1u);
    const size_t span = (size_t)(highest - base_vaddr);

    const int rc = vwine_map(span, &out->mapping);
    if (rc != 0) {
        vwine_logf("vwine: %s needs %zu bytes and MVII would not map them\n",
                   name ? name : "image", span);
        return rc;
    }

    out->span_base = base_vaddr;
    out->span = span;
    out->load_bias = (uintptr_t)out->mapping.base - (uintptr_t)base_vaddr;

    // Pass two: copy. The allocation is already zeroed, so .bss -- the
    // p_memsz-beyond-p_filesz tail -- needs no work.
    for (unsigned i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != VWINE_PT_LOAD || ph[i].p_memsz == 0) continue;
        uint8_t* dst = (uint8_t*)(out->load_bias + (uintptr_t)ph[i].p_vaddr);
        if (ph[i].p_filesz)
            memcpy(dst, blob + ph[i].p_offset, ph[i].p_filesz);
    }

    // PT_DYNAMIC, and the tables it names.
    for (unsigned i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != VWINE_PT_DYNAMIC) continue;
        out->dynamic =
            (const Vwine_Elf32_Dyn*)(out->load_bias + (uintptr_t)ph[i].p_vaddr);
        break;
    }
    out->syment = sizeof(Vwine_Elf32_Sym);
    if (out->dynamic) {
        for (const Vwine_Elf32_Dyn* d = out->dynamic; d->d_tag != VWINE_DT_NULL; ++d) {
            switch (d->d_tag) {
            case VWINE_DT_SYMTAB:
                out->symtab = (const Vwine_Elf32_Sym*)(out->load_bias + d->d_un.d_ptr);
                break;
            case VWINE_DT_STRTAB:
                out->strtab = (const char*)(out->load_bias + d->d_un.d_ptr);
                break;
            case VWINE_DT_STRSZ:   out->strtab_size = d->d_un.d_val; break;
            case VWINE_DT_SYMENT:  out->syment = d->d_un.d_val; break;
            case VWINE_DT_INIT:    out->init_func = out->load_bias + d->d_un.d_ptr; break;
            case VWINE_DT_FINI:    out->fini_func = out->load_bias + d->d_un.d_ptr; break;
            case VWINE_DT_INIT_ARRAY:
                out->init_array = out->load_bias + d->d_un.d_ptr;
                break;
            case VWINE_DT_FINI_ARRAY:
                out->fini_array = out->load_bias + d->d_un.d_ptr;
                break;
            case VWINE_DT_INIT_ARRAYSZ:
                out->init_array_count = d->d_un.d_val / sizeof(uint32_t);
                break;
            case VWINE_DT_FINI_ARRAYSZ:
                out->fini_array_count = d->d_un.d_val / sizeof(uint32_t);
                break;
            default: break;
            }
        }
        if (out->syment == 0) out->syment = sizeof(Vwine_Elf32_Sym);
    }

    out->entry = out->load_bias + (uintptr_t)eh->e_entry;

    vwine_logf("vwine: %s mapped at %p (%zu bytes, bias 0x%08lx, entry %p)\n",
               name ? name : "image", out->mapping.base, span,
               (unsigned long)out->load_bias, (void*)out->entry);
    return 0;
}

// ── relocation ─────────────────────────────────────────────────────────────

// One symbol-bearing relocation's target address, or 0 when the import is not
// implemented (recorded in `missing` by the caller).
static uintptr_t resolve_symbol(const vwine_image* image, uint32_t sym_index,
                                const vwine_registry* registry,
                                const char** out_name)
{
    *out_name = NULL;
    if (sym_index == 0 || !image->symtab) return 0;

    const Vwine_Elf32_Sym* sym =
        (const Vwine_Elf32_Sym*)((const uint8_t*)image->symtab +
                                 (size_t)sym_index * image->syment);

    // Defined in this image: no import, just bias it.
    if (sym->st_shndx != 0) return image->load_bias + (uintptr_t)sym->st_value;

    if (!image->strtab) return 0;
    if (image->strtab_size && sym->st_name >= image->strtab_size) return 0;
    const char* name = image->strtab + sym->st_name;
    *out_name = name;
    if (!registry) return 0;
    return (uintptr_t)vwine_registry_resolve_name(registry, NULL, name);
}

static int apply_rel_table(vwine_image* image, const Vwine_Elf32_Rel* rel,
                           size_t count, const vwine_registry* registry,
                           vwine_missing_set* missing)
{
    for (size_t i = 0; i < count; ++i) {
        const uint32_t type = VWINE_ELF32_R_TYPE(rel[i].r_info);
        const uint32_t sym_index = VWINE_ELF32_R_SYM(rel[i].r_info);

        // The relocation names a link-time address; the slot to patch is that
        // address biased into the mapping.
        const uintptr_t slot_addr = image->load_bias + (uintptr_t)rel[i].r_offset;
        if (slot_addr < (uintptr_t)image->mapping.base ||
            slot_addr + sizeof(uint32_t) >
                (uintptr_t)image->mapping.base + image->mapping.length) {
            vwine_logf("vwine: %s relocation %zu targets 0x%08lx, outside the "
                       "mapped image\n", image->name ? image->name : "image", i,
                       (unsigned long)rel[i].r_offset);
            return -ENOEXEC;
        }
        uint32_t* slot = (uint32_t*)slot_addr;

        switch (type) {
        case VWINE_R_ARM_NONE:
            break;

        case VWINE_R_ARM_RELATIVE:
            // The overwhelmingly common case in a PIE: the slot already holds a
            // link-time address and simply needs the bias added. REL (not RELA)
            // keeps the addend in the slot, which is why this is += and not =.
            *slot += (uint32_t)image->load_bias;
            break;

        case VWINE_R_ARM_ABS32: {
            const char* sym_name = NULL;
            const uintptr_t target =
                resolve_symbol(image, sym_index, registry, &sym_name);
            if (target == 0 && sym_index != 0) {
                vwine_missing_add(missing, image->name, sym_name, 0, 0);
                break;
            }
            *slot += (uint32_t)target;
            break;
        }

        case VWINE_R_ARM_REL32: {
            const char* sym_name = NULL;
            const uintptr_t target =
                resolve_symbol(image, sym_index, registry, &sym_name);
            if (target == 0 && sym_index != 0) {
                vwine_missing_add(missing, image->name, sym_name, 0, 0);
                break;
            }
            *slot += (uint32_t)(target - slot_addr);
            break;
        }

        case VWINE_R_ARM_GLOB_DAT:
        case VWINE_R_ARM_JUMP_SLOT: {
            const char* sym_name = NULL;
            const uintptr_t target =
                resolve_symbol(image, sym_index, registry, &sym_name);
            if (target == 0) {
                vwine_missing_add(missing, image->name, sym_name, 0, 0);
                break;
            }
            // GOT and PLT slots are assigned, not accumulated: the ARM ABI
            // specifies these two as S (+ A only where the addend is zero,
            // which the linker guarantees for these types).
            *slot = (uint32_t)target;
            break;
        }

        default:
            // Deliberately fatal. An unhandled relocation type silently skipped
            // leaves a pointer holding a link-time address, which will be
            // dereferenced later at an address that means nothing -- a crash
            // with no connection to its cause.
            vwine_logf("vwine: %s uses relocation type %u, which this loader "
                       "does not implement (relocation %zu at 0x%08lx)\n",
                       image->name ? image->name : "image", (unsigned)type, i,
                       (unsigned long)rel[i].r_offset);
            return -ENOEXEC;
        }
    }
    return 0;
}

int vwine_elf_relocate(vwine_image* image, const vwine_registry* registry,
                       vwine_missing_set* missing)
{
    if (!image) return -EINVAL;
    if (!image->dynamic) return 0;   // ET_EXEC with no dynamic section

    const Vwine_Elf32_Rel* rel = NULL;
    size_t rel_size = 0;
    size_t rel_entsize = sizeof(Vwine_Elf32_Rel);
    const Vwine_Elf32_Rel* jmprel = NULL;
    size_t jmprel_size = 0;

    for (const Vwine_Elf32_Dyn* d = image->dynamic; d->d_tag != VWINE_DT_NULL; ++d) {
        switch (d->d_tag) {
        case VWINE_DT_REL:
            rel = (const Vwine_Elf32_Rel*)(image->load_bias + d->d_un.d_ptr);
            break;
        case VWINE_DT_RELSZ:    rel_size = d->d_un.d_val; break;
        case VWINE_DT_RELENT:   rel_entsize = d->d_un.d_val; break;
        case VWINE_DT_JMPREL:
            jmprel = (const Vwine_Elf32_Rel*)(image->load_bias + d->d_un.d_ptr);
            break;
        case VWINE_DT_PLTRELSZ: jmprel_size = d->d_un.d_val; break;
        default: break;
        }
    }
    if (rel_entsize == 0) rel_entsize = sizeof(Vwine_Elf32_Rel);
    if (rel_entsize != sizeof(Vwine_Elf32_Rel)) {
        vwine_logf("vwine: %s declares DT_RELENT %zu; ARM ELF32 REL entries are "
                   "%zu bytes\n", image->name ? image->name : "image",
                   rel_entsize, sizeof(Vwine_Elf32_Rel));
        return -ENOEXEC;
    }

    int rc = 0;
    if (rel && rel_size) {
        rc = apply_rel_table(image, rel, rel_size / rel_entsize, registry, missing);
        if (rc != 0) return rc;
    }
    if (jmprel && jmprel_size) {
        rc = apply_rel_table(image, jmprel, jmprel_size / rel_entsize, registry,
                             missing);
    }
    return rc;
}

// ── finalization ───────────────────────────────────────────────────────────

int vwine_image_finalize(vwine_image* image)
{
    if (!image || !image->mapping.base) return -EINVAL;
    return vwine_make_executable(image->mapping.base, image->mapping.length);
}

void vwine_image_run_initializers(const vwine_image* image)
{
    if (!image) return;
    typedef void (*init_fn)(void);

    if (image->init_func) ((init_fn)image->init_func)();
    if (image->init_array && image->init_array_count) {
        const uint32_t* entries = (const uint32_t*)image->init_array;
        for (size_t i = 0; i < image->init_array_count; ++i) {
            // The array holds absolute addresses that DT_REL has already
            // biased, so no further adjustment. A 0 or -1 entry is the
            // documented "skip" convention and appears in real images.
            const uint32_t fn = entries[i];
            if (fn == 0 || fn == 0xFFFFFFFFu) continue;
            ((init_fn)(uintptr_t)fn)();
        }
    }
}

void vwine_image_run_finalizers(const vwine_image* image)
{
    if (!image) return;
    typedef void (*fini_fn)(void);

    if (image->fini_array && image->fini_array_count) {
        const uint32_t* entries = (const uint32_t*)image->fini_array;
        // Reverse order, as the ARM ABI specifies for DT_FINI_ARRAY.
        for (size_t i = image->fini_array_count; i-- > 0;) {
            const uint32_t fn = entries[i];
            if (fn == 0 || fn == 0xFFFFFFFFu) continue;
            ((fini_fn)(uintptr_t)fn)();
        }
    }
    if (image->fini_func) ((fini_fn)image->fini_func)();
}

void vwine_image_release(vwine_image* image)
{
    if (!image) return;
    vwine_unmap(&image->mapping);
    memset(image, 0, sizeof(*image));
}
