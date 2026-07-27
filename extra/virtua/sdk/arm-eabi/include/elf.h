//===-- System V header elf.h ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_ELF_H
#define LLVM_LIBC_ELF_H

#include "__llvm-libc-common.h"
#include "llvm-libc-macros/elf-macros.h"

#include "llvm-libc-types/Elf32_Addr.h"
#include "llvm-libc-types/Elf32_Chdr.h"
#include "llvm-libc-types/Elf32_Dyn.h"
#include "llvm-libc-types/Elf32_Ehdr.h"
#include "llvm-libc-types/Elf32_Half.h"
#include "llvm-libc-types/Elf32_Lword.h"
#include "llvm-libc-types/Elf32_Nhdr.h"
#include "llvm-libc-types/Elf32_Off.h"
#include "llvm-libc-types/Elf32_Phdr.h"
#include "llvm-libc-types/Elf32_Rel.h"
#include "llvm-libc-types/Elf32_Rela.h"
#include "llvm-libc-types/Elf32_Shdr.h"
#include "llvm-libc-types/Elf32_Sword.h"
#include "llvm-libc-types/Elf32_Sym.h"
#include "llvm-libc-types/Elf32_Verdaux.h"
#include "llvm-libc-types/Elf32_Verdef.h"
#include "llvm-libc-types/Elf32_Vernaux.h"
#include "llvm-libc-types/Elf32_Verneed.h"
#include "llvm-libc-types/Elf32_Versym.h"
#include "llvm-libc-types/Elf32_Word.h"
#include "llvm-libc-types/Elf64_Addr.h"
#include "llvm-libc-types/Elf64_Chdr.h"
#include "llvm-libc-types/Elf64_Dyn.h"
#include "llvm-libc-types/Elf64_Ehdr.h"
#include "llvm-libc-types/Elf64_Half.h"
#include "llvm-libc-types/Elf64_Lword.h"
#include "llvm-libc-types/Elf64_Nhdr.h"
#include "llvm-libc-types/Elf64_Off.h"
#include "llvm-libc-types/Elf64_Phdr.h"
#include "llvm-libc-types/Elf64_Rel.h"
#include "llvm-libc-types/Elf64_Rela.h"
#include "llvm-libc-types/Elf64_Shdr.h"
#include "llvm-libc-types/Elf64_Sword.h"
#include "llvm-libc-types/Elf64_Sxword.h"
#include "llvm-libc-types/Elf64_Sym.h"
#include "llvm-libc-types/Elf64_Verdaux.h"
#include "llvm-libc-types/Elf64_Verdef.h"
#include "llvm-libc-types/Elf64_Vernaux.h"
#include "llvm-libc-types/Elf64_Verneed.h"
#include "llvm-libc-types/Elf64_Versym.h"
#include "llvm-libc-types/Elf64_Word.h"
#include "llvm-libc-types/Elf64_Xword.h"

#define DF_BIND_NOW 0x8

#define DF_ORIGIN 0x1

#define DF_STATIC_TLS 0x10

#define DF_SYMBOLIC 0x2

#define DF_TEXTREL 0x4

#define DT_BIND_NOW 24

#define DT_DEBUG 21

#define DT_ENCODING 32

#define DT_FINI 13

#define DT_FINI_ARRAY 26

#define DT_FINI_ARRAYSZ 28

#define DT_FLAGS 30

#define DT_HASH 4

#define DT_HIOS 0x6ffff000

#define DT_HIPROC 0x7fffffff

#define DT_INIT 12

#define DT_INIT_ARRAY 25

#define DT_INIT_ARRAYSZ 27

#define DT_JMPREL 23

#define DT_LOOS 0x6000000D

#define DT_LOPROC 0x70000000

#define DT_NEEDED 1

#define DT_NULL 0

#define DT_PLTGOT 3

#define DT_PLTREL 20

#define DT_PLTRELSZ 2

#define DT_PREINIT_ARRAY 32

#define DT_PREINIT_ARRAYSZ 33

#define DT_REL 17

#define DT_RELA 7

#define DT_RELACOUNT 0x6ffffff9

#define DT_RELAENT 9

#define DT_RELASZ 8

#define DT_RELCOUNT 0x6ffffffa

#define DT_RELENT 19

#define DT_RELSZ 18

#define DT_RPATH 15

#define DT_RUNPATH 29

#define DT_SONAME 14

#define DT_STRSZ 10

#define DT_STRTAB 5

#define DT_SYMBOLIC 16

#define DT_SYMENT 11

#define DT_SYMTAB 6

#define DT_SYMTAB_SHNDX 34

#define DT_TEXTREL 22

#define DT_VERDEF 0x6ffffffc

#define DT_VERDEFNUM 0x6ffffffd

#define DT_VERNEED 0x6ffffffe

#define DT_VERNEEDNUM 0x6fffffff

#define DT_VERSYM 0x6ffffff0

#define EI_ABIVERSION 8

#define EI_CLASS 4

#define EI_DATA 5

#define EI_MAG0 0

#define EI_MAG1 1

#define EI_MAG2 2

#define EI_MAG3 3

#define EI_NIDENT 16

#define EI_NIDENT 16

#define EI_OSABI 7

#define EI_PAD 9

#define EI_VERSION 6

#define ELFCLASS32 1

#define ELFCLASS64 2

#define ELFCLASSNONE 0

#define ELFCOMPRESS_HIOS 0x6fffffff

#define ELFCOMPRESS_HIPROC 0x7fffffff

#define ELFCOMPRESS_LOOS 0x60000000

#define ELFCOMPRESS_LOPROC 0x70000000

#define ELFCOMPRESS_ZLIB 1

#define ELFCOMPRESS_ZSTD 2

#define ELFDATA2LSB 1

#define ELFDATA2MSB 2

#define ELFDATANONE 0

#define ELFMAG "\177ELF"

#define ELFMAG0 0x7f

#define ELFMAG1 'E'

#define ELFMAG2 'L'

#define ELFMAG3 'F'

#define ELFOSABI_LINUX 3

#define ELFOSABI_NONE 0

#define EM_386 3

#define EM_AARCH64 183

#define EM_ARM 40

#define EM_NONE 0

#define EM_RISCV 243

#define EM_X86_64 62

#define ET_CORE 4

#define ET_DYN 3

#define ET_EXEC 2

#define ET_HIOS 0xfeff

#define ET_HIPROC 0xffff

#define ET_LOOS 0xfe00

#define ET_LOPROC 0xff00

#define ET_NONE 0

#define ET_REL 1

#define EV_CURRENT 1

#define EV_NONE 0

#define NT_AUXV 6

#define NT_GNU_ABI_TAG 1

#define NT_GNU_BUILD_ID 3

#define NT_GNU_GOLD_VERSION 4

#define NT_GNU_HWCAP 2

#define NT_GNU_PROPERTY_TYPE_0 5

#define NT_PLATFORM 5

#define NT_PRFPREG 2

#define NT_PRPSINFO 3

#define NT_PRSTATUS 1

#define NT_TASKSTRUCT 4

#define PF_MASKOS 0x0ff00000

#define PF_MASKPROC 0xf0000000

#define PF_R 0x4

#define PF_W 0x2

#define PF_X 0x1

#define PT_DYNAMIC 2

#define PT_HIOS 0x6fffffff

#define PT_HIPROC 0x7fffffff

#define PT_INTERP 3

#define PT_LOAD 1

#define PT_LOOS 0x60000000

#define PT_LOPROC 0x70000000

#define PT_NOTE 4

#define PT_NULL 0

#define PT_PHDR 6

#define PT_SHLIB 5

#define PT_TLS 7

#define SELFMAG 4

#define SHF_ALLOC 0x2

#define SHF_COMPRESSED 0x800

#define SHF_EXECINSTR 0x4

#define SHF_GROUP 0x200

#define SHF_INFO_LINK 0x40

#define SHF_LINK_ORDER 0x80

#define SHF_MASKOS 0x0ff00000

#define SHF_MASKPROC 0xf0000000

#define SHF_MERGE 0x10

#define SHF_OS_NONCONFORMING 0x100

#define SHF_STRINGS 0x20

#define SHF_TLS 0x400

#define SHF_WRITE 0x1

#define SHN_ABS 0xfff1

#define SHN_COMMON 0xfff2

#define SHN_HIOS 0xff3f

#define SHN_HIPROC 0xff1f

#define SHN_HIRESERVE 0xffff

#define SHN_LOOS 0xff20

#define SHN_LOPROC 0xff00

#define SHN_LORESERVE 0xff00

#define SHN_UNDEF 0

#define SHN_XINDEX 0xffff

#define SHT_DYNAMIC 6

#define SHT_DYNSYM 11

#define SHT_FINI_ARRAY 15

#define SHT_GROUP 17

#define SHT_HASH 5

#define SHT_HIOS 0x6fffffff

#define SHT_HIPROC 0x7fffffff

#define SHT_HIUSER 0xffffffff

#define SHT_INIT_ARRAY 14

#define SHT_LOOS 0x60000000

#define SHT_LOPROC 0x70000000

#define SHT_LOUSER 0x80000000

#define SHT_NOBITS 8

#define SHT_NOTE 7

#define SHT_NULL 0

#define SHT_PREINIT_ARRAY 16

#define SHT_PROGBITS 1

#define SHT_REL 9

#define SHT_RELA 4

#define SHT_SHLIB 10

#define SHT_STRTAB 3

#define SHT_SYMTAB 2

#define SHT_SYMTAB_SHNDX 18

#define STB_GLOBAL 1

#define STB_HIOS 12

#define STB_HIPROC 15

#define STB_LOCAL 0

#define STB_LOOS 10

#define STB_LOPROC 13

#define STB_WEAK 2

#define STN_UNDEF 0

#define STT_COMMON 5

#define STT_FILE 4

#define STT_FUNC 2

#define STT_HIOS 12

#define STT_HIPROC 15

#define STT_LOOS 10

#define STT_LOPROC 13

#define STT_NOTYPE 0

#define STT_OBJECT 1

#define STT_SECTION 3

#define STT_TLS 6

#define STV_DEFAULT 0

#define STV_HIDDEN 2

#define STV_INTERNAL 1

#define STV_PROTECTED 3

#define VER_DEF_CURRENT 1

#define VER_DEF_NONE 0

#define VER_DEF_NUM 2

#define VER_FLG_BASE 0x1

#define VER_FLG_WEAK 0x2

#define VER_NDX_ELIMINATE 0xff01

#define VER_NDX_GLOBAL 1

#define VER_NDX_LOCAL 0

#define VER_NDX_LORESERVE 0xff00

__BEGIN_C_DECLS

__END_C_DECLS

#endif // LLVM_LIBC_ELF_H
