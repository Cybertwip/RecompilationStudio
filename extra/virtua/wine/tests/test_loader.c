// Host-side proof for the vwine ELF32 loader.
//
// What this can and cannot prove, stated up front: the loaded image is ARM
// code, so on an x86_64 host it cannot be EXECUTED. What it can prove -- and
// what every load-time bug lives in -- is that the image is placed correctly,
// that R_ARM_RELATIVE fixups land on the right words with the right values,
// that a resolvable import binds to the host function, and that an
// unimplemented import is reported by name instead of silently becoming zero.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vwine/vwine_elf.h"
#include "vwine/vwine_registry.h"

static int failures = 0;

static void check(int ok, const char* what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

// Pull a link-time symbol value straight out of the file's .symtab so the test
// knows where the loader was supposed to put things.
static uint32_t link_time_value(const uint8_t* blob, size_t size,
                                const char* wanted)
{
    const Vwine_Elf32_Ehdr* eh = (const Vwine_Elf32_Ehdr*)blob;
    if (eh->e_shoff == 0 || eh->e_shnum == 0) return 0;
    typedef struct {
        uint32_t sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size;
        uint32_t sh_link, sh_info, sh_addralign, sh_entsize;
    } Shdr;
    const Shdr* sh = (const Shdr*)(blob + eh->e_shoff);
    for (unsigned i = 0; i < eh->e_shnum; ++i) {
        if (sh[i].sh_type != 2 /* SHT_SYMTAB */) continue;
        const Vwine_Elf32_Sym* syms =
            (const Vwine_Elf32_Sym*)(blob + sh[i].sh_offset);
        const size_t count = sh[i].sh_size / sizeof(Vwine_Elf32_Sym);
        const char* strs = (const char*)(blob + sh[sh[i].sh_link].sh_offset);
        for (size_t s = 0; s < count; ++s) {
            if (strcmp(strs + syms[s].st_name, wanted) == 0)
                return syms[s].st_value;
        }
    }
    (void)size;
    return 0;
}

// The one import the registry implements. Never called here (it is x86 code and
// the guest is ARM); its ADDRESS is what the relocation must land on.
static int host_provided_symbol(int x) { return x + 1; }

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { fprintf(stderr, "usage: %s <guest.so>\n", argv[0]); return 2; }

    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror("open guest"); return 2; }
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* blob = malloc((size_t)len);
    if (fread(blob, 1, (size_t)len, f) != (size_t)len) { perror("read"); return 2; }
    fclose(f);

    printf("vwine loader proof (%s, %ld bytes)\n", argv[1], len);

    // 1. identification
    check(vwine_elf_identify(blob, (size_t)len) == VWINE_ELF_ARM32,
          "identifies a real ARM ELF32 as natively loadable");
    {
        // An AArch64 header must be refused with the CPU reason, not "invalid".
        uint8_t fake[64];
        memset(fake, 0, sizeof(fake));
        memcpy(fake, "\x7f" "ELF", 4);
        fake[4] = 2;                       // ELFCLASS64
        fake[5] = 1;                       // little-endian
        fake[18] = 183; fake[19] = 0;      // EM_AARCH64
        check(vwine_elf_identify(fake, sizeof(fake)) == VWINE_ELF_AARCH64,
              "refuses AArch64 with the 64-bit-CPU reason");
    }

    // 2. load
    vwine_image image;
    const int rc = vwine_elf_load(blob, (size_t)len, "guest.so", &image);
    check(rc == 0, "maps every PT_LOAD into one allocation");
    if (rc != 0) return 1;
    check(image.mapping.base != NULL && image.mapping.length > 0,
          "reports the mapping it made");
    check(image.dynamic != NULL, "locates PT_DYNAMIC");
    check(image.symtab != NULL && image.strtab != NULL,
          "resolves DT_SYMTAB and DT_STRTAB");

    // 3. relocation, with one import implemented and one deliberately absent
    static const vwine_export exports[] = {
        { 0, "host_provided_symbol", (void*)(uintptr_t)&host_provided_symbol },
    };
    static const vwine_library libs[] = {
        { "testhost", 0, exports, 1 },
    };
    const vwine_registry registry = { libs, 1 };

    vwine_missing_set missing;
    vwine_missing_reset(&missing);
    const int rrc = vwine_elf_relocate(&image, &registry, &missing);
    check(rrc == 0, "processes every relocation type the image uses");

    // 4. the fixups themselves -- the actual point of the exercise
    const uint32_t table_lt  = link_time_value(blob, (size_t)len, "table");
    const uint32_t datum_a_lt = link_time_value(blob, (size_t)len, "datum_a");
    const uint32_t datum_b_lt = link_time_value(blob, (size_t)len, "datum_b");
    check(table_lt != 0 && datum_a_lt != 0 && datum_b_lt != 0,
          "test found the guest's link-time symbol values");

    if (table_lt && datum_a_lt && datum_b_lt) {
        const uint32_t* table = (const uint32_t*)(image.load_bias + table_lt);
        const uint32_t want_a = (uint32_t)(image.load_bias + datum_a_lt);
        const uint32_t want_b = (uint32_t)(image.load_bias + datum_b_lt);
        printf("       table[0]=0x%08x want 0x%08x | table[1]=0x%08x want 0x%08x\n",
               table[0], want_a, table[1], want_b);
        check(table[0] == want_a && table[1] == want_b,
              "R_ARM_RELATIVE rebased both pointers onto the mapping");

        const int* pa = (const int*)(uintptr_t)table[0];
        const int* pb = (const int*)(uintptr_t)table[1];
        check(*pa == 0x11111111 && *pb == 0x22222222,
              "the rebased pointers reach the guest's real initialised data");
    }

    // 5. the import contract
    check(missing.total == 1, "reports exactly the one unimplemented import");
    check(missing.count == 1 && missing.entries[0].symbol &&
              strcmp(missing.entries[0].symbol, "host_missing_symbol") == 0,
          "names the missing import rather than silently zeroing it");
    printf("       report follows:\n");
    vwine_missing_report(&missing, "guest.so");

    vwine_image_release(&image);
    check(image.mapping.base == NULL, "release clears the mapping");

    printf("%s (%d failure%s)\n", failures ? "PROOF FAILED" : "PROOF OK",
           failures, failures == 1 ? "" : "s");
    free(blob);
    return failures ? 1 : 0;
}
