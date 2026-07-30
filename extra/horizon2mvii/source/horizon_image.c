#include "horizon_image.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "vwine/vwine_log.h"
#include "vwine/vwine_mem.h"

// ── container headers ──────────────────────────────────────────────────────
//
// Laid out byte-for-byte as the formats define them. The padding fields are
// named rather than skipped because an unnamed gap is where a future reader
// puts the wrong offset.

typedef struct nro_segment {
    uint32_t offset;   // file offset, which is also the memory offset
    uint32_t size;
} nro_segment;

typedef struct nro_header {
    uint32_t    entry_branch;          // 0x00: b <start>
    uint32_t    module_header_offset;  // 0x04: -> MOD0
    uint32_t    reserved_08;
    uint32_t    reserved_0c;
    uint32_t    magic;                 // 0x10: 'NRO0'
    uint32_t    version;
    uint32_t    file_size;             // 0x18
    uint32_t    flags;
    nro_segment segments[3];           // 0x20: text, rodata, data
    uint32_t    bss_size;              // 0x38
    uint32_t    reserved_3c;
    uint8_t     build_id[0x20];        // 0x40
    uint8_t     reserved_60[0x20];
} nro_header;

typedef struct nso_segment {
    uint32_t offset;    // file offset of the (possibly compressed) bytes
    uint32_t location;  // where it goes in the image
    uint32_t size;      // decompressed size
    uint32_t extra;     // alignment, or bss_size for segment 2
} nso_segment;

typedef struct nso_header {
    uint32_t    magic;        // 'NSO0'
    uint32_t    version;
    uint32_t    reserved;
    uint32_t    flags;        // bit i: segment i is LZ4-compressed
    nso_segment segments[3];  // text, rodata, data
    uint8_t     build_id[0x20];
    uint32_t    compressed_size[3];
    uint8_t     padding[0x1C];
    uint32_t    api_info_offset, api_info_size;
    uint32_t    dynstr_offset, dynstr_size;
    uint32_t    dynsym_offset, dynsym_size;
    uint8_t     segment_hashes[3][0x20];
} nso_header;

typedef struct mod0_header {
    uint32_t magic;              // 'MOD0'
    int32_t  dynamic_offset;     // all offsets below are relative to THIS header
    int32_t  bss_start_offset;
    int32_t  bss_end_offset;
    int32_t  unwind_start_offset;
    int32_t  unwind_end_offset;
    int32_t  module_offset;
} mod0_header;

// Elf64_Dyn, for the AArch64 probe only. Nothing here loads a 64-bit image; it
// is read solely to be able to say "this is 64-bit" instead of "this failed".
typedef struct elf64_dyn {
    uint64_t d_tag;
    uint64_t d_val;
} elf64_dyn;

#define DT_NULL_64   0
#define DT_RELA_64   7
#define DT_RELASZ_64 8

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(nro_header) == 0x80, "NRO header is 0x80 bytes");
_Static_assert(sizeof(nso_header) == 0x100, "NSO header is 0x100 bytes");
_Static_assert(sizeof(mod0_header) == 0x1C, "MOD0 header is 0x1C bytes");
#endif

static uint32_t read_u32(const uint8_t* p)
{
    // Byte-wise because a container field is not guaranteed to be aligned in
    // the blob we were handed, and -munaligned-access is a build flag, not a
    // promise this file gets to rely on.
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static size_t page_round_up(size_t value)
{
    const size_t page = (size_t)VWINE_PAGE_SIZE;
    if (value > (size_t)-1 - (page - 1u)) return 0;
    return (value + (page - 1u)) & ~(page - 1u);
}

// ── LZ4 ────────────────────────────────────────────────────────────────────
//
// NSO segments are LZ4 *block* format (no frame header, no checksum): a stream
// of (literal run, match) pairs. Written out here rather than linked because
// the whole decoder is sixty lines and adding a third-party archive to a
// freestanding ARM target to get them is not a trade worth making.
//
// Every length accumulator is checked against the remaining input and output
// before it is used. A malformed NSO is a hostile input as far as this is
// concerned: it arrives from a file the user chose.

long horizon_lz4_decompress_block(const uint8_t* src, size_t src_size,
                                  uint8_t* dst, size_t dst_capacity)
{
    if (!src || !dst) return -1;

    size_t ip = 0;
    size_t op = 0;

    while (ip < src_size) {
        const uint8_t token = src[ip++];

        size_t literal_len = (size_t)(token >> 4);
        if (literal_len == 15u) {
            for (;;) {
                if (ip >= src_size) return -1;
                const uint8_t add = src[ip++];
                literal_len += add;
                if (add != 255u) break;
                if (literal_len > dst_capacity) return -1;  // runaway length
            }
        }

        if (literal_len > src_size - ip) return -1;
        if (literal_len > dst_capacity - op) return -1;
        memcpy(dst + op, src + ip, literal_len);
        ip += literal_len;
        op += literal_len;

        // The last sequence in a block is literals only and stops here.
        if (ip == src_size) break;
        if (src_size - ip < 2u) return -1;

        const size_t offset = (size_t)src[ip] | ((size_t)src[ip + 1] << 8);
        ip += 2;
        if (offset == 0 || offset > op) return -1;

        size_t match_len = (size_t)(token & 0x0Fu);
        if (match_len == 15u) {
            for (;;) {
                if (ip >= src_size) return -1;
                const uint8_t add = src[ip++];
                match_len += add;
                if (add != 255u) break;
                if (match_len > dst_capacity) return -1;
            }
        }
        match_len += 4u;  // the minimum match, not encoded

        if (match_len > dst_capacity - op) return -1;
        // Byte at a time: LZ4 matches are allowed to overlap the output cursor
        // (offset < match_len is how it encodes runs), so memcpy/memmove are
        // both wrong here.
        const uint8_t* match = dst + op - offset;
        for (size_t i = 0; i < match_len; ++i) dst[op + i] = match[i];
        op += match_len;
    }

    return (long)op;
}

// ── identification ─────────────────────────────────────────────────────────

horizon_container horizon_image_identify(const void* data, size_t size)
{
    if (!data || size < sizeof(nso_header)) {
        if (!data || size < sizeof(nro_header)) return HORIZON_CONTAINER_UNKNOWN;
    }
    const uint8_t* blob = (const uint8_t*)data;

    if (size >= 4 && read_u32(blob) == HORIZON_NSO_MAGIC) return HORIZON_CONTAINER_NSO;
    if (size >= 0x14 && read_u32(blob + 0x10) == HORIZON_NRO_MAGIC)
        return HORIZON_CONTAINER_NRO;
    return HORIZON_CONTAINER_UNKNOWN;
}

const char* horizon_container_text(horizon_container container)
{
    switch (container) {
    case HORIZON_CONTAINER_NRO: return "NRO0";
    case HORIZON_CONTAINER_NSO: return "NSO0";
    default: return "not an NRO0 or NSO0 module";
    }
}

const char* horizon_arch_text(horizon_arch arch)
{
    switch (arch) {
    case HORIZON_ARCH_ARM32:   return "ARM32 (AArch32)";
    case HORIZON_ARCH_AARCH64: return "AArch64";
    default: return "indeterminate";
    }
}

// Which architecture does the entry branch say this is?
static horizon_arch arch_from_entry_branch(const uint8_t* image, size_t size)
{
    if (size < 4) return HORIZON_ARCH_UNKNOWN;
    const uint32_t word = read_u32(image);

    // AArch64 B: bits 31..26 == 000101. Checked first because the ARM32 test is
    // a byte compare that a 64-bit branch could in principle match.
    if ((word >> 26) == 0x05u) return HORIZON_ARCH_AARCH64;
    // ARM32 B with cond=AL: 1110 101L, L=0.
    if ((word >> 24) == 0xEAu) return HORIZON_ARCH_ARM32;
    return HORIZON_ARCH_UNKNOWN;
}

// ...and what does .dynamic say? Read the same bytes both ways and see which
// interpretation produces a well-formed table.
static horizon_arch arch_from_dynamic(const void* dynamic, size_t bytes_available)
{
    if (!dynamic) return HORIZON_ARCH_UNKNOWN;

    int saw_rel32 = 0;
    int saw_rela64 = 0;

    const Vwine_Elf32_Dyn* d32 = (const Vwine_Elf32_Dyn*)dynamic;
    for (size_t i = 0; (i + 1) * sizeof(*d32) <= bytes_available && i < 4096; ++i) {
        if (d32[i].d_tag == VWINE_DT_NULL) break;
        if (d32[i].d_tag == VWINE_DT_REL || d32[i].d_tag == VWINE_DT_RELSZ) {
            saw_rel32 = 1;
            break;
        }
    }

    const elf64_dyn* d64 = (const elf64_dyn*)dynamic;
    for (size_t i = 0; (i + 1) * sizeof(*d64) <= bytes_available && i < 4096; ++i) {
        if (d64[i].d_tag == DT_NULL_64) break;
        if (d64[i].d_tag == DT_RELA_64 || d64[i].d_tag == DT_RELASZ_64) {
            saw_rela64 = 1;
            break;
        }
    }

    if (saw_rel32 && !saw_rela64) return HORIZON_ARCH_ARM32;
    if (saw_rela64 && !saw_rel32) return HORIZON_ARCH_AARCH64;
    return HORIZON_ARCH_UNKNOWN;
}

horizon_arch horizon_image_probe_arch(const uint8_t* image_base, size_t image_size,
                                      const void* dynamic)
{
    const horizon_arch by_branch = arch_from_entry_branch(image_base, image_size);
    size_t dynamic_bytes = 0;
    if (dynamic && (const uint8_t*)dynamic >= image_base &&
        (const uint8_t*)dynamic < image_base + image_size) {
        dynamic_bytes = (size_t)(image_base + image_size - (const uint8_t*)dynamic);
    }
    const horizon_arch by_dynamic = arch_from_dynamic(dynamic, dynamic_bytes);

    if (by_branch != HORIZON_ARCH_UNKNOWN && by_dynamic != HORIZON_ARCH_UNKNOWN) {
        if (by_branch == by_dynamic) return by_branch;
        vwine_logf("horizon: the entry branch reads as %s but .dynamic reads as "
                   "%s; refusing rather than guessing which is right\n",
                   horizon_arch_text(by_branch), horizon_arch_text(by_dynamic));
        return HORIZON_ARCH_UNKNOWN;
    }
    // One signal is enough when the other is simply absent -- a module with no
    // .dynamic at all is legal, and so is one whose first word we do not
    // recognise as a branch.
    if (by_branch != HORIZON_ARCH_UNKNOWN) return by_branch;
    if (by_dynamic != HORIZON_ARCH_UNKNOWN) return by_dynamic;

    vwine_logf("horizon: neither the entry instruction nor .dynamic identifies "
               "this module's architecture\n");
    return HORIZON_ARCH_UNKNOWN;
}

// ── decoding a container into a flat image ─────────────────────────────────
//
// Both containers produce the same thing: a byte buffer laid out exactly as the
// module expects to see itself, with .bss zeroed at the end. That buffer is
// then handed to vwine_map as one contiguous allocation.

typedef struct flat_image {
    uint8_t* bytes;
    size_t   size;
    uint32_t seg_addr[3];
    uint32_t seg_size[3];
    uint32_t bss_size;
} flat_image;

static void flat_image_free(flat_image* flat)
{
    if (flat->bytes) free(flat->bytes);
    flat->bytes = NULL;
    flat->size = 0;
}

static int decode_nro(const uint8_t* blob, size_t size, flat_image* flat)
{
    if (size < sizeof(nro_header)) {
        vwine_logf("horizon: NRO is %zu bytes, shorter than its own header\n", size);
        return -ENOEXEC;
    }
    nro_header hdr;
    memcpy(&hdr, blob, sizeof(hdr));

    if (hdr.file_size > size) {
        vwine_logf("horizon: NRO declares %u bytes but only %zu were read\n",
                   hdr.file_size, size);
        return -ENOEXEC;
    }

    for (int i = 0; i < 3; ++i) {
        if ((size_t)hdr.segments[i].offset + hdr.segments[i].size > hdr.file_size) {
            vwine_logf("horizon: NRO segment %d (offset %u, size %u) runs past the "
                       "declared file size %u\n", i, hdr.segments[i].offset,
                       hdr.segments[i].size, hdr.file_size);
            return -ENOEXEC;
        }
    }

    // An NRO is already laid out as it wants to be mapped: segment offsets are
    // memory offsets. So the image is the file, plus .bss.
    const size_t body = page_round_up(hdr.file_size);
    const size_t bss = page_round_up(hdr.bss_size);
    if (body == 0 || (bss == 0 && hdr.bss_size != 0)) return -ENOMEM;
    const size_t total = body + bss;

    flat->bytes = (uint8_t*)calloc(1, total);
    if (!flat->bytes) return -ENOMEM;
    flat->size = total;
    memcpy(flat->bytes, blob, hdr.file_size);

    for (int i = 0; i < 3; ++i) {
        flat->seg_addr[i] = hdr.segments[i].offset;
        flat->seg_size[i] = hdr.segments[i].size;
    }
    flat->bss_size = (uint32_t)bss;
    return 0;
}

static int decode_nso(const uint8_t* blob, size_t size, flat_image* flat)
{
    if (size < sizeof(nso_header)) {
        vwine_logf("horizon: NSO is %zu bytes, shorter than its own header\n", size);
        return -ENOEXEC;
    }
    nso_header hdr;
    memcpy(&hdr, blob, sizeof(hdr));

    // Highest byte any segment claims, so one allocation covers all three.
    size_t highest = 0;
    for (int i = 0; i < 3; ++i) {
        const size_t end = (size_t)hdr.segments[i].location + hdr.segments[i].size;
        if (end < (size_t)hdr.segments[i].location) return -ENOEXEC;  // wrapped
        if (end > highest) highest = end;
    }
    const size_t bss = page_round_up(hdr.segments[2].extra);
    const size_t body = page_round_up(highest);
    if (body == 0 || (bss == 0 && hdr.segments[2].extra != 0)) return -ENOMEM;
    const size_t total = body + bss;

    flat->bytes = (uint8_t*)calloc(1, total);
    if (!flat->bytes) return -ENOMEM;
    flat->size = total;

    for (int i = 0; i < 3; ++i) {
        const uint32_t stored = hdr.compressed_size[i];
        const uint32_t at = hdr.segments[i].offset;
        if ((size_t)at + stored > size) {
            vwine_logf("horizon: NSO segment %d reads %u bytes at offset %u, past "
                       "the %zu-byte file\n", i, stored, at, size);
            flat_image_free(flat);
            return -ENOEXEC;
        }
        uint8_t* dst = flat->bytes + hdr.segments[i].location;

        if ((hdr.flags >> i) & 1u) {
            const long produced = horizon_lz4_decompress_block(
                blob + at, stored, dst, hdr.segments[i].size);
            if (produced < 0 || (uint32_t)produced != hdr.segments[i].size) {
                vwine_logf("horizon: NSO segment %d decompressed to %ld bytes, not "
                           "the declared %u\n", i, produced, hdr.segments[i].size);
                flat_image_free(flat);
                return -ENOEXEC;
            }
        } else {
            if (stored != hdr.segments[i].size) {
                vwine_logf("horizon: NSO segment %d is stored uncompressed but its "
                           "stored size %u differs from its size %u\n",
                           i, stored, hdr.segments[i].size);
                flat_image_free(flat);
                return -ENOEXEC;
            }
            memcpy(dst, blob + at, stored);
        }

        flat->seg_addr[i] = hdr.segments[i].location;
        flat->seg_size[i] = hdr.segments[i].size;
    }

    flat->bss_size = (uint32_t)bss;
    return 0;
}

// ── load ───────────────────────────────────────────────────────────────────

int horizon_image_load(const void* data, size_t size, const char* name,
                       const vwine_registry* registry, vwine_missing_set* missing,
                       horizon_module* out)
{
    if (!data || !out) return -EINVAL;
    memset(out, 0, sizeof(*out));

    const uint8_t* blob = (const uint8_t*)data;
    out->container = horizon_image_identify(data, size);

    flat_image flat;
    memset(&flat, 0, sizeof(flat));

    int rc;
    switch (out->container) {
    case HORIZON_CONTAINER_NRO: rc = decode_nro(blob, size, &flat); break;
    case HORIZON_CONTAINER_NSO: rc = decode_nso(blob, size, &flat); break;
    default:
        vwine_logf("horizon: %s is %s\n", name ? name : "the image",
                   horizon_container_text(out->container));
        return -ENOEXEC;
    }
    if (rc != 0) return rc;

    // MOD0. The word at image+4 is its offset from the image base; the offsets
    // inside it are relative to the MOD0 header itself, not to the base.
    const void* dynamic_in_flat = NULL;
    int32_t bss_start = 0, bss_end = 0, module_offset = 0;
    int have_mod0 = 0;

    if (flat.size >= 8) {
        const uint32_t mod0_at = read_u32(flat.bytes + 4);
        if ((size_t)mod0_at + sizeof(mod0_header) <= flat.size) {
            mod0_header mod;
            memcpy(&mod, flat.bytes + mod0_at, sizeof(mod));
            if (mod.magic == HORIZON_MOD0_MAGIC) {
                have_mod0 = 1;
                const int64_t dyn_at = (int64_t)mod0_at + mod.dynamic_offset;
                if (dyn_at >= 0 && (size_t)dyn_at < flat.size)
                    dynamic_in_flat = flat.bytes + dyn_at;
                bss_start = (int32_t)((int64_t)mod0_at + mod.bss_start_offset);
                bss_end = (int32_t)((int64_t)mod0_at + mod.bss_end_offset);
                module_offset = (int32_t)((int64_t)mod0_at + mod.module_offset);
            }
        }
    }
    if (!have_mod0) {
        // Legal, and it means the module has no relocations and no imports --
        // but it also means nothing below can be cross-checked, so say so.
        vwine_logf("horizon: %s has no MOD0 header; loading it as a fixed-address "
                   "module with no dynamic section\n", name ? name : "the image");
    }

    out->arch = horizon_image_probe_arch(flat.bytes, flat.size, dynamic_in_flat);
    if (out->arch != HORIZON_ARCH_ARM32) {
        vwine_logf("horizon: %s is %s. This front-end executes the guest's own "
                   "instructions on the device's Cortex-A7, which is ARMv7-A and "
                   "cannot decode AArch64 at all -- there is nothing to fall back "
                   "to. Running it would require a full 64-bit CPU emulator, "
                   "which this is deliberately not.\n",
                   name ? name : "the image", horizon_arch_text(out->arch));
        flat_image_free(&flat);
        return -ENOEXEC;
    }

    rc = vwine_map(flat.size, &out->image.mapping);
    if (rc != 0) {
        vwine_logf("horizon: %s needs %zu bytes and MVII would not map them\n",
                   name ? name : "the image", flat.size);
        flat_image_free(&flat);
        return rc;
    }
    memcpy(out->image.mapping.base, flat.bytes, flat.size);

    // A Horizon module's own addresses start at 0 and the mapping is where it
    // actually landed, so the bias is simply the mapping address.
    out->image.load_bias = (uintptr_t)out->image.mapping.base;
    out->image.span_base = 0;
    out->image.span = flat.size;
    out->image.name = name;
    out->image.entry = out->image.load_bias;  // the entry branch at offset 0
    if (dynamic_in_flat) {
        const size_t dyn_off = (size_t)((const uint8_t*)dynamic_in_flat - flat.bytes);
        out->image.dynamic =
            (const Vwine_Elf32_Dyn*)((uint8_t*)out->image.mapping.base + dyn_off);
    }
    vwine_image_scan_dynamic(&out->image);

    out->text_base = out->image.load_bias + flat.seg_addr[0];
    out->text_size = flat.seg_size[0];
    out->rodata_base = out->image.load_bias + flat.seg_addr[1];
    out->rodata_size = flat.seg_size[1];
    out->data_base = out->image.load_bias + flat.seg_addr[2];
    out->data_size = (size_t)flat.seg_size[2] + flat.bss_size;
    if (have_mod0 && bss_end > bss_start) {
        out->bss_base = out->image.load_bias + (uintptr_t)bss_start;
        out->bss_size = (size_t)(bss_end - bss_start);
    } else {
        out->bss_base = out->image.load_bias + flat.seg_addr[2] + flat.seg_size[2];
        out->bss_size = flat.bss_size;
    }
    if (have_mod0) out->module_object = out->image.load_bias + (uintptr_t)module_offset;

    flat_image_free(&flat);

    vwine_logf("horizon: %s (%s, %s) mapped at %p, %zu bytes, entry %p\n",
               name ? name : "image", horizon_container_text(out->container),
               horizon_arch_text(out->arch), out->image.mapping.base,
               out->image.mapping.length, (void*)out->image.entry);

    rc = vwine_elf_relocate(&out->image, registry, missing);
    if (rc != 0) {
        horizon_image_release(out);
        return rc;
    }
    return 0;
}

void horizon_image_release(horizon_module* module)
{
    if (!module) return;
    vwine_image_release(&module->image);
    memset(module, 0, sizeof(*module));
}
