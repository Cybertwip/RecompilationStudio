#!/usr/bin/env python3
"""Inspect an ELF the PowerEngine toolchain produced.

The compiler bundle at VIRTUA_LLVM_ROOT ships compiler, compiler++, ld.lld,
llvm-ar, llvm-objcopy, llvm-ranlib and strip -- no llvm-nm and no llvm-readelf.
macOS has no readelf either, and its nm cannot read ELF. So there is no way to
answer "did that section survive --gc-sections", "is anything still undefined"
or "what float ABI is this object tagged with" from a shell, and those are
exactly the questions an ARMv7 Virtua link raises.

Usage:
    elfinfo.py sections <elf> [pattern]
    elfinfo.py symbols  <elf> [pattern]
    elfinfo.py undef    <elf>
    elfinfo.py armattrs <elf>

`pattern` is a plain substring, matched case-sensitively.
Exit status is 1 when a query matches nothing, so it composes into a proof
script without needing the output parsed.
"""

import struct
import sys

SHT_SYMTAB = 2
SHT_NOBITS = 8

SHN_UNDEF = 0

STB = {0: "LOCAL", 1: "GLOBAL", 2: "WEAK"}
STT = {0: "NOTYPE", 1: "OBJECT", 2: "FUNC", 3: "SECTION", 4: "FILE", 6: "TLS"}


class Elf:
    def __init__(self, path):
        with open(path, "rb") as handle:
            self.data = handle.read()
        if self.data[:4] != b"\x7fELF":
            raise ValueError(f"{path} is not an ELF file")

        self.is64 = self.data[4] == 2
        self.little = self.data[5] == 1
        self.end = "<" if self.little else ">"

        # e_shoff / e_shentsize / e_shnum / e_shstrndx, at different offsets in
        # the two classes.
        if self.is64:
            self.shoff = self.u(0x28, "Q")
            self.shentsize = self.u(0x3A, "H")
            self.shnum = self.u(0x3C, "H")
            self.shstrndx = self.u(0x3E, "H")
        else:
            self.shoff = self.u(0x20, "I")
            self.shentsize = self.u(0x2E, "H")
            self.shnum = self.u(0x30, "H")
            self.shstrndx = self.u(0x32, "H")

        self.sections = [self._section(i) for i in range(self.shnum)]

        shstr = self.sections[self.shstrndx]
        for sec in self.sections:
            sec["name"] = self._str(shstr["offset"], sec["name_off"])

    def u(self, off, fmt):
        size = struct.calcsize(fmt)
        return struct.unpack_from(self.end + fmt, self.data, off)[0]

    def _section(self, index):
        base = self.shoff + index * self.shentsize
        if self.is64:
            fields = struct.unpack_from(self.end + "IIQQQQIIQQ", self.data, base)
        else:
            fields = struct.unpack_from(self.end + "IIIIIIIIII", self.data, base)
        return {
            "name_off": fields[0],
            "type": fields[1],
            "flags": fields[2],
            "addr": fields[3],
            "offset": fields[4],
            "size": fields[5],
            "link": fields[6],
            "info": fields[7],
            "align": fields[8],
            "entsize": fields[9],
        }

    def _str(self, table_off, index):
        end = self.data.index(b"\0", table_off + index)
        return self.data[table_off + index:end].decode("utf-8", "replace")

    def symbols(self):
        """Every symbol in every symtab, newest-first is not a concern here."""
        out = []
        for sec in self.sections:
            if sec["type"] != SHT_SYMTAB:
                continue
            strtab = self.sections[sec["link"]]["offset"]
            entsize = sec["entsize"] or (24 if self.is64 else 16)
            count = sec["size"] // entsize
            for i in range(count):
                base = sec["offset"] + i * entsize
                if self.is64:
                    name_off, info, other, shndx, value, size = struct.unpack_from(
                        self.end + "IBBHQQ", self.data, base)
                else:
                    name_off, value, size, info, other, shndx = struct.unpack_from(
                        self.end + "IIIBBH", self.data, base)
                name = self._str(strtab, name_off)
                if not name:
                    continue
                out.append({
                    "name": name,
                    "value": value,
                    "size": size,
                    "bind": STB.get(info >> 4, str(info >> 4)),
                    "type": STT.get(info & 0xF, str(info & 0xF)),
                    "shndx": shndx,
                })
        return out


def cmd_sections(elf, pattern):
    rows = [s for s in elf.sections if s["name"] and (not pattern or pattern in s["name"])]
    for sec in rows:
        kind = "NOBITS" if sec["type"] == SHT_NOBITS else "PROGBITS/other"
        print(f"{sec['name']:<32} addr=0x{sec['addr']:08x} size={sec['size']:<8} {kind}")
    return bool(rows)


def cmd_symbols(elf, pattern):
    rows = [s for s in elf.symbols() if not pattern or pattern in s["name"]]
    rows.sort(key=lambda s: s["name"])
    for sym in rows:
        where = "UND" if sym["shndx"] == SHN_UNDEF else f"sec{sym['shndx']}"
        print(f"0x{sym['value']:08x} {sym['bind']:<6} {sym['type']:<7} {where:<7} {sym['name']}")
    return bool(rows)


def cmd_undef(elf):
    # Undefined *and* not weak: a weak undefined symbol resolves to zero by
    # design and is not a link failure, so reporting it alongside the real ones
    # would make this command useless for the check it exists for.
    rows = [s for s in elf.symbols() if s["shndx"] == SHN_UNDEF and s["bind"] != "WEAK"]
    for sym in sorted(rows, key=lambda s: s["name"]):
        print(f"UND {sym['bind']:<6} {sym['name']}")
    return bool(rows)


def cmd_armattrs(elf):
    """Decode the one ARM attribute that decides whether objects can be linked.

    Tag_ABI_VFP_args (28): 0 = base AAPCS (soft-float calling convention),
    1 = VFP registers (hard float). Mixing the two in one link is the failure
    this command exists to catch, and it is silent at every other stage.
    """
    sec = next((s for s in elf.sections if s["name"] == ".ARM.attributes"), None)
    if sec is None:
        print("no .ARM.attributes section")
        return False

    blob = elf.data[sec["offset"]:sec["offset"] + sec["size"]]
    if not blob or blob[0] != ord("A"):
        print("unrecognised .ARM.attributes format")
        return False

    pos = 1
    while pos + 4 <= len(blob):
        (seclen,) = struct.unpack_from(elf.end + "I", blob, pos)
        if seclen < 5 or pos + seclen > len(blob):
            break
        end = blob.index(b"\0", pos + 4)
        vendor = blob[pos + 4:end].decode("ascii", "replace")
        body = blob[end + 1:pos + seclen]
        if vendor == "aeabi":
            _decode_aeabi(body)
        pos += seclen
    return True


def _uleb(buf, pos):
    result = 0
    shift = 0
    while True:
        byte = buf[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return result, pos
        shift += 7


# Attributes whose value is a NUL-terminated string rather than a ULEB128.
_STRING_TAGS = {4, 5, 67}

_NAMES = {
    6: "Tag_CPU_arch",
    7: "Tag_CPU_arch_profile",
    9: "Tag_THUMB_ISA_use",
    10: "Tag_FP_arch",
    18: "Tag_ABI_PCS_wchar_t",
    20: "Tag_ABI_FP_denormal",
    21: "Tag_ABI_FP_exceptions",
    23: "Tag_ABI_FP_number_model",
    24: "Tag_ABI_align_needed",
    25: "Tag_ABI_align_preserved",
    26: "Tag_ABI_enum_size",
    28: "Tag_ABI_VFP_args",
    34: "Tag_CPU_unaligned_access",
}


def _decode_aeabi(body):
    pos = 0
    while pos < len(body):
        tag, pos = _uleb(body, pos)
        if pos + 4 > len(body):
            return
        (size,) = struct.unpack_from("<I", body, pos)
        sub = body[pos + 4:pos + size]
        pos += size
        if tag != 1:  # 1 = Tag_File; per-section/per-symbol scopes are unused here
            continue
        inner = 0
        while inner < len(sub):
            attr, inner = _uleb(sub, inner)
            if attr in _STRING_TAGS:
                end = sub.index(b"\0", inner)
                value = sub[inner:end].decode("ascii", "replace")
                inner = end + 1
            else:
                value, inner = _uleb(sub, inner)
            name = _NAMES.get(attr)
            if attr == 28:
                meaning = "soft-float AAPCS" if value == 0 else "hard-float (VFP registers)"
                print(f"Tag_ABI_VFP_args = {value} ({meaning})")
            elif name:
                print(f"{name} = {value}")


def main(argv):
    if len(argv) < 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    command, path = argv[1], argv[2]
    pattern = argv[3] if len(argv) > 3 else ""
    elf = Elf(path)

    if command == "sections":
        found = cmd_sections(elf, pattern)
    elif command == "symbols":
        found = cmd_symbols(elf, pattern)
    elif command == "undef":
        # Inverted on purpose: no undefined symbols is the good outcome, so this
        # is the one query that succeeds by matching nothing.
        return 1 if cmd_undef(elf) else 0
    elif command == "armattrs":
        found = cmd_armattrs(elf)
    else:
        print(f"unknown command: {command}", file=sys.stderr)
        return 2

    return 0 if found else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
