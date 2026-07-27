package main

import (
	"bytes"
	"debug/elf"
	"encoding/binary"
	"testing"
)

func testSection(name string, flags elf.SectionFlag, addr, size uint64) *elf.Section {
	return &elf.Section{
		SectionHeader: elf.SectionHeader{
			Name:  name,
			Flags: flags,
			Addr:  addr,
			Size:  size,
		},
	}
}

func TestSupportsMachineAndGlobalPointerRequirements(t *testing.T) {
	if !supportsMachine(elf.EM_RISCV) {
		t.Fatalf("expected riscv64 to be supported")
	}
	if !supportsMachine(elf.EM_X86_64) {
		t.Fatalf("expected x86_64 to be supported")
	}
	if !supportsMachine(elf.EM_AARCH64) {
		t.Fatalf("expected aarch64 to be supported")
	}
	if !supportsMachine(elf.EM_ARM) {
		t.Fatalf("expected ARM to be supported")
	}
	if !requiresGlobalPointer(elf.EM_RISCV) {
		t.Fatalf("expected riscv64 to require a global pointer")
	}
	if requiresGlobalPointer(elf.EM_X86_64) {
		t.Fatalf("did not expect x86_64 to require a global pointer")
	}
	if requiresGlobalPointer(elf.EM_AARCH64) {
		t.Fatalf("did not expect aarch64 to require a global pointer")
	}
}

func TestCollectAllocSectionsSkipsZeroSizedAllocSections(t *testing.T) {
	file := &elf.File{
		Sections: []*elf.Section{
			testSection(".text", elf.SHF_ALLOC, 0x1000, 0x100),
			testSection(".bss", elf.SHF_ALLOC, 0x2000, 0x0),
			testSection(".rodata", elf.SHF_ALLOC, 0x1800, 0x80),
			testSection(".debug_info", 0, 0x0, 0x40),
		},
	}

	base, max, sections, err := collectAllocSections(file)
	if err != nil {
		t.Fatalf("collectAllocSections failed: %v", err)
	}
	if base != 0x1000 {
		t.Fatalf("base = 0x%x, want 0x1000", base)
	}
	if max != 0x1880 {
		t.Fatalf("max = 0x%x, want 0x1880", max)
	}
	if len(sections) != 2 {
		t.Fatalf("len(sections) = %d, want 2", len(sections))
	}
}

func TestCollectAllocSectionsAlignsImageBase(t *testing.T) {
	file := &elf.File{
		Sections: []*elf.Section{
			testSection(".note.go.buildid", elf.SHF_ALLOC, 0x400f78, 0x64),
			testSection(".text", elf.SHF_ALLOC, 0x401000, 0x100),
		},
	}

	base, max, _, err := collectAllocSections(file)
	if err != nil {
		t.Fatalf("collectAllocSections failed: %v", err)
	}
	if base != 0x400000 {
		t.Fatalf("base = 0x%x, want 0x400000", base)
	}
	if max != 0x401100 {
		t.Fatalf("max = 0x%x, want 0x401100", max)
	}
}

func TestShouldProcessRelocationSectionUsesELFSectionIndex(t *testing.T) {
	file := &elf.File{
		Sections: []*elf.Section{
			testSection(".data.rel.ro", elf.SHF_ALLOC, 0x2000, 0x80),
			testSection(".comment", 0, 0, 0x20),
		},
	}
	relocSection := &elf.Section{
		SectionHeader: elf.SectionHeader{
			Name: ".rela.data.rel.ro",
			Type: elf.SHT_RELA,
			Info: 1,
		},
	}

	if !shouldProcessRelocationSection(file, relocSection) {
		t.Fatalf("expected relocation section targeting ELF section index 1 to be processed")
	}
}

func TestSectionByELFIndexSkipsNullSection(t *testing.T) {
	text := testSection(".text", elf.SHF_ALLOC, 0x1000, 0x100)
	data := testSection(".data", elf.SHF_ALLOC, 0x2000, 0x100)
	file := &elf.File{Sections: []*elf.Section{text, data}}

	if got := sectionByELFIndex(file, 0); got != nil {
		t.Fatalf("sectionByELFIndex(..., 0) = %v, want nil", got)
	}
	if got := sectionByELFIndex(file, 1); got != text {
		t.Fatalf("sectionByELFIndex(..., 1) = %v, want .text", got)
	}
	if got := sectionByELFIndex(file, 2); got != data {
		t.Fatalf("sectionByELFIndex(..., 2) = %v, want .data", got)
	}
	if got := sectionByELFIndex(file, 3); got != nil {
		t.Fatalf("sectionByELFIndex(..., 3) = %v, want nil", got)
	}
}

func TestStaticDataPointerRelocationsSkipZeroForZeroBasedImages(t *testing.T) {
	blob := make([]byte, 32)
	binary.LittleEndian.PutUint64(blob[8:16], 0x10)
	file := &elf.File{
		FileHeader: elf.FileHeader{ByteOrder: binary.LittleEndian},
		Sections: []*elf.Section{
			testSection(".data", elf.SHF_ALLOC|elf.SHF_WRITE, 0, uint64(len(blob))),
		},
	}
	seen := map[uint64]struct{}{}
	var offsets []uint64

	recordStaticDataPointerRelocations(file, 0, blob, seen, &offsets)

	if _, ok := seen[0]; ok {
		t.Fatalf("zero-valued data word was treated as a relocation")
	}
	if len(offsets) != 1 || offsets[0] != 8 {
		t.Fatalf("offsets = %#v, want [8]", offsets)
	}
	if got := binary.LittleEndian.Uint64(blob[8:16]); got != 0x10 {
		t.Fatalf("relocated pointer = 0x%x, want 0x10", got)
	}
}

func TestStaticDataPointerRelocationsScanGoReadOnlyMetadata(t *testing.T) {
	const base = 0x400000
	blob := make([]byte, 0x1000)
	binary.LittleEndian.PutUint64(blob[0x80:0x88], base+0x220)
	file := &elf.File{
		FileHeader: elf.FileHeader{ByteOrder: binary.LittleEndian},
		Sections: []*elf.Section{
			testSection(".note.go.buildid", elf.SHF_ALLOC, base, 0x20),
			testSection(".rodata", elf.SHF_ALLOC, base, uint64(len(blob))),
		},
	}
	seen := map[uint64]struct{}{}
	var offsets []uint64

	recordStaticDataPointerRelocations(file, base, blob, seen, &offsets)

	if len(offsets) != 1 || offsets[0] != 0x80 {
		t.Fatalf("offsets = %#v, want [0x80]", offsets)
	}
	if got := binary.LittleEndian.Uint64(blob[0x80:0x88]); got != 0x220 {
		t.Fatalf("relocated read-only Go pointer = 0x%x, want 0x220", got)
	}
}

func TestStaticDataPointerRelocationsAllowOnePastImageEnd(t *testing.T) {
	const base = 0x400000
	blob := make([]byte, 0x100)
	binary.LittleEndian.PutUint64(blob[0x40:0x48], base+uint64(len(blob)))
	file := &elf.File{
		FileHeader: elf.FileHeader{ByteOrder: binary.LittleEndian},
		Sections: []*elf.Section{
			testSection(".data", elf.SHF_ALLOC|elf.SHF_WRITE, base, uint64(len(blob))),
		},
	}
	seen := map[uint64]struct{}{}
	var offsets []uint64

	recordStaticDataPointerRelocations(file, base, blob, seen, &offsets)

	if len(offsets) != 1 || offsets[0] != 0x40 {
		t.Fatalf("offsets = %#v, want [0x40]", offsets)
	}
	if got := binary.LittleEndian.Uint64(blob[0x40:0x48]); got != uint64(len(blob)) {
		t.Fatalf("relocated end pointer = 0x%x, want 0x%x", got, len(blob))
	}
}

func TestRecordGoPclnTextStartRelocation(t *testing.T) {
	const base = 0x400000
	const pclnAddr = 0xc8d000
	blob := make([]byte, pclnAddr-base+64)
	pclnOffset := pclnAddr - base
	binary.LittleEndian.PutUint32(blob[pclnOffset:pclnOffset+4], 0xfffffff1)
	binary.LittleEndian.PutUint64(blob[pclnOffset+24:pclnOffset+32], 0x401000)
	file := &elf.File{
		FileHeader: elf.FileHeader{ByteOrder: binary.LittleEndian},
		Sections: []*elf.Section{
			testSection(".gopclntab", elf.SHF_ALLOC, pclnAddr, 64),
		},
	}
	seen := map[uint64]struct{}{}
	var offsets []uint64

	recordGoPclnTextStartRelocation(file, base, blob, seen, &offsets)

	wantOffset := uint64(pclnOffset + 24)
	if len(offsets) != 1 || offsets[0] != wantOffset {
		t.Fatalf("offsets = %#v, want [0x%x]", offsets, wantOffset)
	}
	if got := binary.LittleEndian.Uint64(blob[pclnOffset+24 : pclnOffset+32]); got != 0x1000 {
		t.Fatalf("textStart = 0x%x, want image-relative 0x1000", got)
	}
}

func TestFindGlobalPointerFallsBackToSectionLayout(t *testing.T) {
	file := &elf.File{
		Sections: []*elf.Section{
			testSection(".text", elf.SHF_ALLOC, 0x1000, 0x400),
			testSection(".data", elf.SHF_ALLOC, 0x2000, 0x100),
			testSection(".sdata", elf.SHF_ALLOC, 0x2800, 0x20),
			testSection(".sbss", elf.SHF_ALLOC, 0x3000, 0x100),
			testSection(".bss", elf.SHF_ALLOC, 0x3400, 0x400),
		},
	}

	if got := findGlobalPointer(file, 0x1000); got != 0x2000 {
		t.Fatalf("findGlobalPointer() = 0x%x, want 0x2000", got)
	}
}

func TestRelocationAbsoluteValueRiscv64UsesSymbolAndAddend(t *testing.T) {
	file := &elf.File{FileHeader: elf.FileHeader{Machine: elf.EM_RISCV}}
	syms := []elf.Symbol{{Value: 0x5000}}

	value, ok := relocationAbsoluteValue(file, relocation{
		Type:   rRiscv64,
		Sym:    1,
		Addend: 0x20,
	}, 0, syms)
	if !ok {
		t.Fatalf("relocationAbsoluteValue returned !ok")
	}
	if value != 0x5020 {
		t.Fatalf("value = 0x%x, want 0x5020", value)
	}
}

func TestRelocationAbsoluteValueX8664HandlesRelativeAndAbs64(t *testing.T) {
	file := &elf.File{FileHeader: elf.FileHeader{Machine: elf.EM_X86_64}}
	syms := []elf.Symbol{{Value: 0x4000}}

	relValue, ok := relocationAbsoluteValue(file, relocation{
		Type:   rX8664Relative,
		Addend: 0x1234,
	}, 0, nil)
	if !ok {
		t.Fatalf("relative relocation returned !ok")
	}
	if relValue != 0x1234 {
		t.Fatalf("relative value = 0x%x, want 0x1234", relValue)
	}

	absValue, ok := relocationAbsoluteValue(file, relocation{
		Type:   rX8664Abs64,
		Sym:    1,
		Addend: 0x10,
	}, 0, syms)
	if !ok {
		t.Fatalf("abs64 relocation returned !ok")
	}
	if absValue != 0x4010 {
		t.Fatalf("abs64 value = 0x%x, want 0x4010", absValue)
	}
}

func TestRelocationAbsoluteValueX8664KeepsZeroValuedDefinedSymbolAddend(t *testing.T) {
	file := &elf.File{FileHeader: elf.FileHeader{Machine: elf.EM_X86_64}}
	syms := []elf.Symbol{{Value: 0, Section: 1}}

	value, ok := relocationAbsoluteValue(file, relocation{
		Type:   rX8664Abs64,
		Sym:    1,
		Addend: 0x1c7,
	}, 0, syms)
	if !ok {
		t.Fatalf("abs64 relocation returned !ok")
	}
	if value != 0x1c7 {
		t.Fatalf("abs64 value = 0x%x, want 0x1c7", value)
	}
}

func TestRelocationAbsoluteValueAarch64HandlesRelativeAndAbs64(t *testing.T) {
	file := &elf.File{FileHeader: elf.FileHeader{Machine: elf.EM_AARCH64}}
	syms := []elf.Symbol{{Value: 0x8000}}

	relValue, ok := relocationAbsoluteValue(file, relocation{
		Type:   rAarch64Relative,
		Addend: 0x2345,
	}, 0, nil)
	if !ok {
		t.Fatalf("relative relocation returned !ok")
	}
	if relValue != 0x2345 {
		t.Fatalf("relative value = 0x%x, want 0x2345", relValue)
	}

	absValue, ok := relocationAbsoluteValue(file, relocation{
		Type:   rAarch64Abs64,
		Sym:    1,
		Addend: 0x18,
	}, 0, syms)
	if !ok {
		t.Fatalf("abs64 relocation returned !ok")
	}
	if absValue != 0x8018 {
		t.Fatalf("abs64 value = 0x%x, want 0x8018", absValue)
	}
}

func TestDecodeRelocationsElf32ArmRelAndRela(t *testing.T) {
	relData := make([]byte, 8)
	binary.LittleEndian.PutUint32(relData[0:4], 0x1200)
	binary.LittleEndian.PutUint32(relData[4:8], (7<<8)|rArmAbs32)
	rel := &elf.Section{
		SectionHeader: elf.SectionHeader{Name: ".rel.data", Type: elf.SHT_REL, Entsize: 8, Size: uint64(len(relData))},
		ReaderAt:      bytesReaderAt(relData),
	}
	relaData := make([]byte, 12)
	binary.LittleEndian.PutUint32(relaData[0:4], 0x1300)
	binary.LittleEndian.PutUint32(relaData[4:8], rArmRelative)
	binary.LittleEndian.PutUint32(relaData[8:12], 0xfffffffc)
	rela := &elf.Section{
		SectionHeader: elf.SectionHeader{Name: ".rela.dyn", Type: elf.SHT_RELA, Entsize: 12, Size: uint64(len(relaData))},
		ReaderAt:      bytesReaderAt(relaData),
	}
	file := &elf.File{FileHeader: elf.FileHeader{Class: elf.ELFCLASS32, ByteOrder: binary.LittleEndian, Machine: elf.EM_ARM}}

	relocs, err := decodeRelocations(file, rel)
	if err != nil {
		t.Fatalf("decode REL failed: %v", err)
	}
	if len(relocs) != 1 || relocs[0].Offset != 0x1200 || relocs[0].Sym != 7 || relocs[0].Type != rArmAbs32 {
		t.Fatalf("REL decode = %#v", relocs)
	}
	relocs, err = decodeRelocations(file, rela)
	if err != nil {
		t.Fatalf("decode RELA failed: %v", err)
	}
	if len(relocs) != 1 || relocs[0].Offset != 0x1300 || relocs[0].Type != rArmRelative || relocs[0].Addend != -4 {
		t.Fatalf("RELA decode = %#v", relocs)
	}
}

func TestProcessRelocationsArm32PatchesWords(t *testing.T) {
	const base = 0x8000
	blob := make([]byte, 0x200)
	binary.LittleEndian.PutUint32(blob[0x40:0x44], base+0x180)
	binary.LittleEndian.PutUint32(blob[0x80:0x84], 0x120)

	relData := make([]byte, 16)
	binary.LittleEndian.PutUint32(relData[0:4], base+0x40)
	binary.LittleEndian.PutUint32(relData[4:8], rArmRelative)
	binary.LittleEndian.PutUint32(relData[8:12], base+0x80)
	binary.LittleEndian.PutUint32(relData[12:16], rArmRelative)

	data := testSection(".data", elf.SHF_ALLOC|elf.SHF_WRITE, base, uint64(len(blob)))
	rel := &elf.Section{
		SectionHeader: elf.SectionHeader{
			Name:    ".rel.data",
			Type:    elf.SHT_REL,
			Entsize: 8,
			Size:    uint64(len(relData)),
			Info:    1,
		},
		ReaderAt: bytesReaderAt(relData),
	}
	file := &elf.File{
		FileHeader: elf.FileHeader{Class: elf.ELFCLASS32, ByteOrder: binary.LittleEndian, Machine: elf.EM_ARM},
		Sections:   []*elf.Section{data, rel},
	}

	offsets, err := processRelocations(file, base, blob)
	if err != nil {
		t.Fatalf("processRelocations failed: %v", err)
	}
	if len(offsets) != 2 || offsets[0] != 0x40 || offsets[1] != 0x80 {
		t.Fatalf("offsets = %#v, want [0x40 0x80]", offsets)
	}
	if got := binary.LittleEndian.Uint32(blob[0x40:0x44]); got != 0x180 {
		t.Fatalf("relative patched word = 0x%x, want 0x180", got)
	}
	if got := binary.LittleEndian.Uint32(blob[0x80:0x84]); got != 0x120 {
		t.Fatalf("second patched word = 0x%x, want 0x120", got)
	}
}

func TestProcessRelocationsArm32RecordsMovwMovtPair(t *testing.T) {
	const base = 0x8000
	const target = base + 0x1234
	blob := make([]byte, 0x200)
	binary.LittleEndian.PutUint32(blob[0x40:0x44], armMovWithImm16(0xe3002000, uint16(target&0xffff)))
	binary.LittleEndian.PutUint32(blob[0x54:0x58], armMovWithImm16(0xe3402000, uint16(target>>16)))

	relData := make([]byte, 16)
	binary.LittleEndian.PutUint32(relData[0:4], base+0x40)
	binary.LittleEndian.PutUint32(relData[4:8], (1<<8)|rArmMovwAbsNC)
	binary.LittleEndian.PutUint32(relData[8:12], base+0x54)
	binary.LittleEndian.PutUint32(relData[12:16], (1<<8)|rArmMovtAbs)

	text := testSection(".text", elf.SHF_ALLOC|elf.SHF_EXECINSTR, base, uint64(len(blob)))
	rel := &elf.Section{
		SectionHeader: elf.SectionHeader{
			Name:    ".rel.text",
			Type:    elf.SHT_REL,
			Entsize: 8,
			Size:    uint64(len(relData)),
			Info:    1,
		},
		ReaderAt: bytesReaderAt(relData),
	}
	file := &elf.File{
		FileHeader: elf.FileHeader{Class: elf.ELFCLASS32, ByteOrder: binary.LittleEndian, Machine: elf.EM_ARM},
		Sections:   []*elf.Section{text, rel},
	}

	offsets, err := processRelocations(file, base, blob)
	if err != nil {
		t.Fatalf("processRelocations failed: %v", err)
	}
	if len(offsets) != 1 {
		t.Fatalf("offsets = %#v, want one MOVW/MOVT record", offsets)
	}
	if kind := offsets[0] >> virtuaRelocKindShift; kind != virtuaRelocKindArmMovwMovt {
		t.Fatalf("relocation kind = %d, want ARM MOVW/MOVT", kind)
	}
	movwOffset, movtOffset := unpackArmMovwMovtRelocation(offsets[0])
	if movwOffset != 0x40 || movtOffset != 0x54 {
		t.Fatalf("packed offsets = 0x%x/0x%x, want 0x40/0x54", movwOffset, movtOffset)
	}

	movw := binary.LittleEndian.Uint32(blob[0x40:0x44])
	movt := binary.LittleEndian.Uint32(blob[0x54:0x58])
	got := (uint32(armMovImm16(movt)) << 16) | uint32(armMovImm16(movw))
	if got != 0x1234 {
		t.Fatalf("rewritten MOVW/MOVT value = 0x%x, want 0x1234", got)
	}
}

func TestRecordArmGOTSlotRelocations(t *testing.T) {
	const base = 0x8000
	const target = base + 0x180
	blob := make([]byte, 0x300)
	binary.LittleEndian.PutUint32(blob[0x120:0x124], target)
	binary.LittleEndian.PutUint32(blob[0x140:0x144], target)
	binary.LittleEndian.PutUint32(blob[0x160:0x164], base+0x1c0)

	data := testSection(".data", elf.SHF_ALLOC|elf.SHF_WRITE, base+0x100, 0x100)
	file := &elf.File{
		FileHeader: elf.FileHeader{Class: elf.ELFCLASS32, ByteOrder: binary.LittleEndian, Machine: elf.EM_ARM},
		Sections:   []*elf.Section{data},
	}
	seen := map[uint64]struct{}{0x140: {}}
	var offsets []uint64
	recordArmGOTSlotRelocations(file,
		base,
		blob,
		map[uint64]struct{}{target: {}},
		seen,
		&offsets)

	if len(offsets) != 1 || offsets[0] != 0x120 {
		t.Fatalf("offsets = %#v, want [0x120]", offsets)
	}
	if got := binary.LittleEndian.Uint32(blob[0x120:0x124]); got != 0x180 {
		t.Fatalf("GOT slot = 0x%x, want image-relative 0x180", got)
	}
	if got := binary.LittleEndian.Uint32(blob[0x140:0x144]); got != target {
		t.Fatalf("seen pointer changed to 0x%x, want 0x%x", got, target)
	}
	if got := binary.LittleEndian.Uint32(blob[0x160:0x164]); got != base+0x1c0 {
		t.Fatalf("unrelated word changed to 0x%x", got)
	}
}

func TestRecordArmGOTSlotRelocationsRecordsZeroBasedSlot(t *testing.T) {
	const target = 0x180
	blob := make([]byte, 0x300)
	binary.LittleEndian.PutUint32(blob[0x120:0x124], target)

	data := testSection(".data", elf.SHF_ALLOC|elf.SHF_WRITE, 0x100, 0x100)
	file := &elf.File{
		FileHeader: elf.FileHeader{Class: elf.ELFCLASS32, ByteOrder: binary.LittleEndian, Machine: elf.EM_ARM},
		Sections:   []*elf.Section{data},
	}
	seen := make(map[uint64]struct{})
	var offsets []uint64
	recordArmGOTSlotRelocations(file,
		0,
		blob,
		map[uint64]struct{}{target: {}},
		seen,
		&offsets)

	if len(offsets) != 1 || offsets[0] != 0x120 {
		t.Fatalf("offsets = %#v, want [0x120]", offsets)
	}
	if got := binary.LittleEndian.Uint32(blob[0x120:0x124]); got != target {
		t.Fatalf("zero-based GOT slot = 0x%x, want unchanged relative value 0x%x", got, target)
	}
}

func TestRelocationAbsoluteValueArm32HandlesRelativeAndAbs32(t *testing.T) {
	file := &elf.File{FileHeader: elf.FileHeader{Machine: elf.EM_ARM}}
	syms := []elf.Symbol{{Value: 0x9000}}

	relValue, ok := relocationAbsoluteValue(file, relocation{Type: rArmRelative}, 0x1234, nil)
	if !ok {
		t.Fatalf("relative relocation returned !ok")
	}
	if relValue != 0x1234 {
		t.Fatalf("relative value = 0x%x, want 0x1234", relValue)
	}

	absValue, ok := relocationAbsoluteValue(file, relocation{
		Type:   rArmAbs32,
		Sym:    1,
		Addend: 0x20,
	}, 0, syms)
	if !ok {
		t.Fatalf("abs32 relocation returned !ok")
	}
	if absValue != 0x9020 {
		t.Fatalf("abs32 value = 0x%x, want 0x9020", absValue)
	}

	target1Value, ok := relocationAbsoluteValue(file, relocation{
		Type:   rArmTarget1,
		Sym:    1,
		Addend: 0x34,
	}, 0, syms)
	if !ok {
		t.Fatalf("target1 relocation returned !ok")
	}
	if target1Value != 0x9034 {
		t.Fatalf("target1 value = 0x%x, want 0x9034", target1Value)
	}
}

func TestRelocationAbsoluteValueArm32UsesImplicitRelAddend(t *testing.T) {
	file := &elf.File{FileHeader: elf.FileHeader{Machine: elf.EM_ARM}}
	syms := []elf.Symbol{{Value: 0, Section: 1}}

	value, ok := relocationAbsoluteValue(file, relocation{
		Type: rArmAbs32,
		Sym:  1,
	}, 0x30d6a4, syms)
	if !ok {
		t.Fatalf("abs32 relocation returned !ok")
	}
	if value != 0x30d6a4 {
		t.Fatalf("abs32 value = 0x%x, want implicit REL addend 0x30d6a4", value)
	}
}

func TestProcessRelocationsArm32RecordsInitArrayPointerWithoutRelocation(t *testing.T) {
	blob := make([]byte, 0x1100)
	binary.LittleEndian.PutUint32(blob[0x1000:0x1004], 0x80)
	binary.LittleEndian.PutUint32(blob[0x1004:0x1008], 0)
	binary.LittleEndian.PutUint32(blob[0x1008:0x100c], 0x2000)

	text := testSection(".text", elf.SHF_ALLOC|elf.SHF_EXECINSTR, 0, 0x400)
	initArray := testSection(".init_array", elf.SHF_ALLOC|elf.SHF_WRITE, 0x1000, 0x0c)
	file := &elf.File{
		FileHeader: elf.FileHeader{Class: elf.ELFCLASS32, ByteOrder: binary.LittleEndian, Machine: elf.EM_ARM},
		Sections:   []*elf.Section{text, initArray},
	}

	offsets, err := processRelocations(file, 0, blob)
	if err != nil {
		t.Fatalf("processRelocations failed: %v", err)
	}
	if len(offsets) != 1 || offsets[0] != 0x1000 {
		t.Fatalf("offsets = %#v, want [0x1000]", offsets)
	}
	if got := binary.LittleEndian.Uint32(blob[0x1000:0x1004]); got != 0x80 {
		t.Fatalf("init array patched word = 0x%x, want 0x80", got)
	}
}

func bytesReaderAt(data []byte) *bytes.Reader {
	return bytes.NewReader(data)
}

func TestCooperativeSchedulerFlagIsIndependentFromAppModeAndArchitecture(t *testing.T) {
	flags := virtuaFlagGUI | virtuaFlagCooperativeScheduler | archFlag(elf.EM_ARM)
	if flags&virtuaFlagGUI == 0 {
		t.Fatalf("GUI flag was lost")
	}
	if flags&virtuaFlagCooperativeScheduler == 0 {
		t.Fatalf("cooperative scheduler flag was lost")
	}
	if flags&virtuaFlagArchMask != virtuaFlagArchArm32 {
		t.Fatalf("architecture flags = 0x%x, want ARM32", flags&virtuaFlagArchMask)
	}
}
