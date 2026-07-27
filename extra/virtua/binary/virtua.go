package main

import (
	"debug/elf"
	"encoding/binary"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

const (
	virtuaMagic    uint32 = 0x56495254
	virtuaVersion  uint32 = 3
	dynamicMagic   uint32 = 0x4e594456 // "VDYN"
	dynamicVersion uint32 = 1
	virtuaFlagGUI  uint64 = 1 << 0
	// Explicit opt-in for applications whose main loop reaches the Virtua
	// yield/sleep ABI. Cooperative kernels cannot safely execute an untagged
	// forever-loop because it can monopolize the OS until the next syscall.
	virtuaFlagCooperativeScheduler uint64 = 1 << 1
	virtuaBaseAlign                uint64 = 0x1000

	rRiscv64           uint32 = 2
	rRiscvRelative     uint32 = 3
	rX8664Abs64        uint32 = 1
	rX8664GlobDat      uint32 = 6
	rX8664JumpSlot     uint32 = 7
	rX8664Relative     uint32 = 8
	rX8664GOTPCREL     uint32 = 9
	rX8664GOTOFF64     uint32 = 25
	rX8664GOT64        uint32 = 27
	rX8664GOTPC64      uint32 = 29
	rX8664GOTPCRELX    uint32 = 41
	rX8664RexGOTPCRELX uint32 = 42

	rAarch64Abs64    uint32 = 257
	rAarch64GlobDat  uint32 = 1025
	rAarch64JumpSlot uint32 = 1026
	rAarch64Relative uint32 = 1027

	rArmAbs32     uint32 = 2
	rArmMovwAbsNC uint32 = 43
	rArmMovtAbs   uint32 = 44
	rArmTarget1   uint32 = 38
	rArmGotPrel   uint32 = 96
	rArmGlobDat   uint32 = 21
	rArmJumpSlot  uint32 = 22
	rArmRelative  uint32 = 23

	virtuaFlagArchShift   uint64 = 8
	virtuaFlagArchMask    uint64 = 0xff << virtuaFlagArchShift
	virtuaFlagArchX8664   uint64 = 1 << virtuaFlagArchShift
	virtuaFlagArchRiscv64 uint64 = 2 << virtuaFlagArchShift
	virtuaFlagArchAarch64 uint64 = 3 << virtuaFlagArchShift
	virtuaFlagArchArm32   uint64 = 4 << virtuaFlagArchShift

	virtuaRelocKindShift       uint64 = 60
	virtuaRelocPayloadMask     uint64 = (uint64(1) << virtuaRelocKindShift) - 1
	virtuaRelocKindPointer     uint64 = 0
	virtuaRelocKindArmMovwMovt uint64 = 1
	virtuaArmMovRelocBits      uint64 = 30
	virtuaArmMovRelocMask      uint64 = (uint64(1) << virtuaArmMovRelocBits) - 1
)

type virtuaHeader struct {
	Magic       uint32
	Version     uint32
	EntryOffset uint64
	BlobSize    uint64
	RelocCount  uint64
	BSSSize     uint64
	GPOffset    uint64
	Flags       uint64
}

type dynamicHeader struct {
	Magic            uint32
	Version          uint32
	BlobSize         uint64
	RelocCount       uint64
	ExportCount      uint64
	ExportStringSize uint64
	BSSSize          uint64
}

type dynamicExport struct {
	NameOffset  uint32
	Reserved    uint32
	ValueOffset uint64
}

type relocation struct {
	Offset uint64
	Type   uint32
	Sym    uint32
	Addend int64
}

type armMovRelocation struct {
	BlobOffset uint64
	Type       uint32
	Sym        uint32
	Rd         uint8
	Imm        uint16
}

func main() {
	kind := flag.String("kind", "auto", "output kind: auto, exec, dynamic")
	appMode := flag.String("app-mode", "auto", "application mode for executables: auto, console, gui")
	scheduler := flag.String("scheduler", "legacy", "scheduler contract for executables: legacy, cooperative")
	flag.Parse()
	args := flag.Args()
	if len(args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: virtua [-kind exec|dynamic] [-app-mode auto|console|gui] [-scheduler legacy|cooperative] <input.elf> <output.virtua|output.dynamic>")
		os.Exit(1)
	}

	if err := writeVirtua(args[0], args[1], *kind, *appMode, *scheduler); err != nil {
		fmt.Fprintln(os.Stderr, "virtua:", err)
		os.Exit(1)
	}
}

func writeVirtua(inputPath, outputPath, kind, appMode, scheduler string) error {
	file, err := elf.Open(inputPath)
	if err != nil {
		return fmt.Errorf("open ELF: %w", err)
	}
	defer file.Close()

	if kind == "auto" || kind == "" {
		if strings.EqualFold(filepath.Ext(outputPath), ".dynamic") {
			kind = "dynamic"
		} else {
			kind = "exec"
		}
	}
	if kind != "exec" && kind != "dynamic" {
		return fmt.Errorf("unknown output kind %q", kind)
	}
	if appMode == "" {
		appMode = "auto"
	}
	if appMode != "auto" && appMode != "console" && appMode != "gui" {
		return fmt.Errorf("unknown app mode %q", appMode)
	}
	if kind == "dynamic" && appMode != "auto" {
		return fmt.Errorf("-app-mode is only valid for executable Virtua outputs")
	}
	if scheduler == "" {
		scheduler = "legacy"
	}
	if scheduler != "legacy" && scheduler != "cooperative" {
		return fmt.Errorf("unknown scheduler contract %q", scheduler)
	}
	if kind == "dynamic" && scheduler != "legacy" {
		return fmt.Errorf("-scheduler is only valid for executable Virtua outputs")
	}

	if file.Class != elf.ELFCLASS64 && !(file.Class == elf.ELFCLASS32 && file.Machine == elf.EM_ARM) {
		return fmt.Errorf("unsupported ELF class %v", file.Class)
	}
	if !supportsMachine(file.Machine) {
		return fmt.Errorf("unsupported Virtua machine %v", file.Machine)
	}

	baseVMA, maxVMA, allocSections, err := collectAllocSections(file)
	if err != nil {
		return err
	}
	if kind == "exec" && file.Entry < baseVMA {
		return fmt.Errorf("entrypoint 0x%x is below base VMA 0x%x", file.Entry, baseVMA)
	}

	blobSize := maxVMA - baseVMA
	if blobSize == 0 {
		return errors.New("ELF contains no allocatable payload")
	}
	if blobSize > uint64(^uint(0)>>1) {
		return fmt.Errorf("allocatable blob is too large: %d bytes", blobSize)
	}

	blob := make([]byte, int(blobSize))
	for _, section := range allocSections {
		if section.Type == elf.SHT_NOBITS {
			continue
		}

		data, err := section.Data()
		if err != nil {
			return fmt.Errorf("read section %s: %w", section.Name, err)
		}

		start := section.Addr - baseVMA
		end := start + uint64(len(data))
		if end > uint64(len(blob)) {
			return fmt.Errorf("section %s overruns Virtua blob", section.Name)
		}
		copy(blob[int(start):int(end)], data)
	}

	relocOffsets, err := processRelocations(file, baseVMA, blob)
	if err != nil {
		return err
	}

	gpOffset := uint64(0)
	if requiresGlobalPointer(file.Machine) {
		gpOffset = findGlobalPointer(file, baseVMA)
		if gpOffset == 0 {
			return errors.New("could not find or infer __global_pointer$; do not run with gp=0. Build unstripped or keep __global_pointer$ in the symbol table")
		}
		if gpOffset >= blobSize {
			return fmt.Errorf("computed gp offset 0x%x is outside blob size 0x%x", gpOffset, blobSize)
		}
	}

	if kind == "dynamic" {
		return writeDynamicOutput(outputPath, file, blob, relocOffsets, baseVMA)
	}

	resolvedAppMode, err := resolveExecutableAppMode(file, baseVMA, uint64(len(blob)), blob, appMode)
	if err != nil {
		return err
	}
	flags := uint64(0)
	if resolvedAppMode == "gui" {
		flags |= virtuaFlagGUI
	}
	if scheduler == "cooperative" {
		flags |= virtuaFlagCooperativeScheduler
	}
	flags |= archFlag(file.Machine)

	entryOffset := uint64(0)
	if file.Entry >= baseVMA {
		entryOffset = file.Entry - baseVMA
	}
	header := virtuaHeader{
		Magic:       virtuaMagic,
		Version:     virtuaVersion,
		EntryOffset: entryOffset,
		BlobSize:    uint64(len(blob)),
		RelocCount:  uint64(len(relocOffsets)),
		BSSSize:     findSectionSize(file, ".bss") + findSectionSize(file, ".sbss"),
		GPOffset:    gpOffset,
		Flags:       flags,
	}

	outDir := filepath.Dir(outputPath)
	if outDir != "." {
		if err := os.MkdirAll(outDir, 0o755); err != nil {
			return fmt.Errorf("create output directory: %w", err)
		}
	}

	output, err := os.Create(outputPath)
	if err != nil {
		return fmt.Errorf("create output: %w", err)
	}
	defer output.Close()

	if err := binary.Write(output, binary.LittleEndian, header); err != nil {
		return fmt.Errorf("write Virtua header: %w", err)
	}
	if _, err := output.Write(blob); err != nil {
		return fmt.Errorf("write Virtua blob: %w", err)
	}
	if len(relocOffsets) != 0 {
		if err := binary.Write(output, binary.LittleEndian, relocOffsets); err != nil {
			return fmt.Errorf("write relocation table: %w", err)
		}
	}

	fmt.Printf("wrote %s (%d relocations, entry=0x%x, gp_offset=0x%x, app_mode=%s, scheduler=%s)\n", outputPath, len(relocOffsets), header.EntryOffset, header.GPOffset, resolvedAppMode, scheduler)
	return nil
}

func resolveExecutableAppMode(file *elf.File, baseVMA, blobSize uint64, blob []byte, requested string) (string, error) {
	if requested == "console" || requested == "gui" {
		return requested, nil
	}
	if hasStrongCallableSymbol(file, "virtua", baseVMA, blobSize) {
		return "gui", nil
	}
	if mode := goRuntimeVirtuaAppMode(file, baseVMA, blob); mode != "" {
		if mode == "gui" || mode == "console" {
			return mode, nil
		}
		return "", fmt.Errorf("unsupported Go Virtua app mode %q", mode)
	}
	if hasVirtuaModeNote(file, "gui") {
		return "gui", nil
	}
	return "console", nil
}

func hasStrongCallableSymbol(file *elf.File, name string, baseVMA, blobSize uint64) bool {
	for _, symbol := range allSymbols(file) {
		if symbol.Name != name || symbol.Section == elf.SHN_UNDEF || symbol.Value < baseVMA || symbol.Value >= baseVMA+blobSize {
			continue
		}
		if elf.ST_BIND(symbol.Info) != elf.STB_GLOBAL {
			continue
		}
		typ := elf.ST_TYPE(symbol.Info)
		if typ == elf.STT_FUNC || typ == elf.STT_NOTYPE {
			return true
		}
	}
	return false
}

func goRuntimeVirtuaAppMode(file *elf.File, baseVMA uint64, blob []byte) string {
	if pointerSize(file) != 8 {
		return ""
	}
	for _, symbol := range allSymbols(file) {
		if symbol.Name != "runtime.virtuaAppMode" || symbol.Section == elf.SHN_UNDEF || symbol.Value < baseVMA {
			continue
		}
		offset := symbol.Value - baseVMA
		if offset+16 > uint64(len(blob)) {
			continue
		}
		ptr := file.ByteOrder.Uint64(blob[offset : offset+8])
		length := file.ByteOrder.Uint64(blob[offset+8 : offset+16])
		if ptr >= baseVMA {
			ptr -= baseVMA
		}
		if length > 64 || ptr+length > uint64(len(blob)) {
			continue
		}
		return string(blob[ptr : ptr+length])
	}
	return ""
}

func hasVirtuaModeNote(file *elf.File, mode string) bool {
	for _, name := range []string{".note.virtua.appmode", ".note.go.virtua-appmode"} {
		section := file.Section(name)
		if section == nil {
			continue
		}
		data, err := section.Data()
		if err != nil {
			continue
		}
		if strings.Contains(strings.ToLower(string(data)), mode) {
			return true
		}
	}
	return false
}

func writeDynamicOutput(outputPath string, file *elf.File, blob []byte, relocOffsets []uint64, baseVMA uint64) error {
	exports, stringsBlob, err := collectDynamicExports(file, baseVMA, uint64(len(blob)))
	if err != nil {
		return err
	}
	if len(exports) == 0 {
		return errors.New("dynamic library exports no global symbols")
	}

	outDir := filepath.Dir(outputPath)
	if outDir != "." {
		if err := os.MkdirAll(outDir, 0o755); err != nil {
			return fmt.Errorf("create output directory: %w", err)
		}
	}

	output, err := os.Create(outputPath)
	if err != nil {
		return fmt.Errorf("create output: %w", err)
	}
	defer output.Close()

	header := dynamicHeader{
		Magic:            dynamicMagic,
		Version:          dynamicVersion,
		BlobSize:         uint64(len(blob)),
		RelocCount:       uint64(len(relocOffsets)),
		ExportCount:      uint64(len(exports)),
		ExportStringSize: uint64(len(stringsBlob)),
		BSSSize:          findSectionSize(file, ".bss") + findSectionSize(file, ".sbss"),
	}
	if err := binary.Write(output, binary.LittleEndian, header); err != nil {
		return fmt.Errorf("write dynamic header: %w", err)
	}
	if _, err := output.Write(blob); err != nil {
		return fmt.Errorf("write dynamic blob: %w", err)
	}
	if len(relocOffsets) != 0 {
		if err := binary.Write(output, binary.LittleEndian, relocOffsets); err != nil {
			return fmt.Errorf("write dynamic relocation table: %w", err)
		}
	}
	if err := binary.Write(output, binary.LittleEndian, exports); err != nil {
		return fmt.Errorf("write dynamic export table: %w", err)
	}
	if _, err := output.Write(stringsBlob); err != nil {
		return fmt.Errorf("write dynamic export strings: %w", err)
	}

	fmt.Printf("wrote %s (%d relocations, %d exports)\n", outputPath, len(relocOffsets), len(exports))
	return nil
}

func collectDynamicExports(file *elf.File, baseVMA, blobSize uint64) ([]dynamicExport, []byte, error) {
	symbols, err := file.Symbols()
	if err != nil {
		symbols, err = file.DynamicSymbols()
	}
	if err != nil {
		return nil, nil, fmt.Errorf("read symbols for dynamic exports: %w", err)
	}

	type namedExport struct {
		name  string
		value uint64
	}
	seen := make(map[string]struct{})
	named := make([]namedExport, 0, 32)
	for _, symbol := range symbols {
		if symbol.Name == "" || symbol.Section == elf.SHN_UNDEF || symbol.Value == 0 {
			continue
		}
		bind := elf.ST_BIND(symbol.Info)
		if bind != elf.STB_GLOBAL && bind != elf.STB_WEAK {
			continue
		}
		typ := elf.ST_TYPE(symbol.Info)
		if typ != elf.STT_FUNC && typ != elf.STT_OBJECT && typ != elf.STT_NOTYPE {
			continue
		}
		if symbol.Value < baseVMA || symbol.Value >= baseVMA+blobSize {
			continue
		}
		if _, ok := seen[symbol.Name]; ok {
			continue
		}
		seen[symbol.Name] = struct{}{}
		named = append(named, namedExport{name: symbol.Name, value: symbol.Value - baseVMA})
	}
	sort.Slice(named, func(i, j int) bool { return named[i].name < named[j].name })

	stringsBlob := make([]byte, 1)
	exports := make([]dynamicExport, 0, len(named))
	for _, export := range named {
		if len(stringsBlob) > int(^uint32(0)) {
			return nil, nil, errors.New("dynamic export string table too large")
		}
		nameOffset := uint32(len(stringsBlob))
		stringsBlob = append(stringsBlob, export.name...)
		stringsBlob = append(stringsBlob, 0)
		exports = append(exports, dynamicExport{
			NameOffset:  nameOffset,
			ValueOffset: export.value,
		})
	}
	return exports, stringsBlob, nil
}

func allSymbols(file *elf.File) []elf.Symbol {
	var out []elf.Symbol
	if symbols, err := file.Symbols(); err == nil {
		out = append(out, symbols...)
	}
	if symbols, err := file.DynamicSymbols(); err == nil {
		out = append(out, symbols...)
	}
	return out
}

func collectAllocSections(file *elf.File) (uint64, uint64, []*elf.Section, error) {
	baseVMA := ^uint64(0)
	var maxVMA uint64
	var allocSections []*elf.Section

	for _, section := range file.Sections {
		if section.Flags&elf.SHF_ALLOC == 0 || section.Size == 0 {
			continue
		}

		allocSections = append(allocSections, section)
		if section.Addr < baseVMA {
			baseVMA = section.Addr
		}

		end := section.Addr + section.Size
		if end > maxVMA {
			maxVMA = end
		}
	}

	if len(allocSections) == 0 {
		return 0, 0, nil, errors.New("ELF contains no allocatable sections")
	}

	return alignDown(baseVMA, virtuaBaseAlign), maxVMA, allocSections, nil
}

func alignDown(value, alignment uint64) uint64 {
	if alignment == 0 {
		return value
	}
	return value &^ (alignment - 1)
}

func processRelocations(file *elf.File, baseVMA uint64, blob []byte) ([]uint64, error) {
	ptrSize := pointerSize(file)
	seen := make(map[uint64]struct{})
	offsets := make([]uint64, 0, 64)
	armMovRelocs := make([]armMovRelocation, 0, 64)
	armGOTTargets := make(map[uint64]struct{})

	for _, section := range file.Sections {
		if section.Type != elf.SHT_RELA && section.Type != elf.SHT_REL {
			continue
		}
		if !shouldProcessRelocationSection(file, section) {
			continue
		}

		relocations, err := decodeRelocations(file, section)
		if err != nil {
			return nil, fmt.Errorf("decode %s: %w", section.Name, err)
		}
		syms, _ := symbolsForRelocationSection(file, section)

		for _, reloc := range relocations {
			if file.Machine == elf.EM_ARM && reloc.Type == rArmGotPrel {
				if symValue, undefined, ok := relocationSymbolValue(reloc, syms); ok && !undefined {
					armGOTTargets[symValue] = struct{}{}
				}
				continue
			}
			if file.Machine == elf.EM_ARM && isArmMovwMovtRelocation(reloc.Type) {
				if mov, ok := decodeArmMovRelocation(file, reloc, baseVMA, blob); ok {
					armMovRelocs = append(armMovRelocs, mov)
				}
				continue
			}
			if file.Machine == elf.EM_X86_64 && isX8664GOTPCRELRelocation(reloc.Type) {
				added, err := recordX8664GOTSlotRelocation(file, reloc, baseVMA, blob, syms, seen, &offsets)
				if err != nil {
					return nil, err
				}
				if added {
					continue
				}
			}
			if file.Machine == elf.EM_X86_64 && reloc.Type == rX8664GOT64 {
				added, err := recordX8664GOT64SlotRelocation(file, reloc, baseVMA, blob, syms, seen, &offsets)
				if err != nil {
					return nil, err
				}
				if added {
					continue
				}
			}

			if !isSupportedRelocation(file.Machine, reloc.Type) || reloc.Offset < baseVMA {
				continue
			}

			blobOffset := reloc.Offset - baseVMA
			if blobOffset+ptrSize > uint64(len(blob)) {
				continue
			}
			if _, exists := seen[blobOffset]; exists {
				continue
			}

			start := int(blobOffset)
			end := start + int(ptrSize)
			existing := readPointer(file, blob[start:end])

			absValue, ok := relocationAbsoluteValue(file, reloc, existing, syms)
			if !ok {
				continue
			}

			// Store an image-relative pointer in the blob. The runtime loader then adds
			// the final load base. Null/undefined symbols intentionally remain zero and
			// are not placed in the relocation table.
			if absValue == 0 {
				continue
			}
			imageRelative := absValue
			if absValue >= baseVMA {
				imageRelative = absValue - baseVMA
			}

			writePointer(file, blob[start:end], imageRelative)
			seen[blobOffset] = struct{}{}
			offsets = append(offsets, blobOffset)
		}
	}

	if err := recordArmMovwMovtRelocations(file, baseVMA, blob, armMovRelocs, seen, &offsets); err != nil {
		return nil, err
	}
	recordArmGOTSlotRelocations(file, baseVMA, blob, armGOTTargets, seen, &offsets)

	// The heuristic pointer scan guesses which 8-byte words are relocatable
	// pointers; it is only needed for ET_EXEC images that carry no relocation
	// table. ET_DYN (PIE) images have a precise .rela/.rel table covering every
	// absolute pointer, so the heuristic is both unnecessary and dangerous
	// there — it misreads Go stack maps in .rodata as pointers and corrupts
	// them (the compiler then dies with "invalid pointer found on stack" while
	// growing a goroutine stack). Skip it whenever real relocations apply.
	if baseVMA != 0 && file.Type != elf.ET_DYN {
		recordStaticDataPointerRelocations(file, baseVMA, blob, seen, &offsets)
	}
	recordArmInitArrayPointerRelocations(file, baseVMA, blob, seen, &offsets)
	recordGoPclnTextStartRelocation(file, baseVMA, blob, seen, &offsets)

	sort.Slice(offsets, func(i, j int) bool { return offsets[i] < offsets[j] })
	return offsets, nil
}

func isArmMovwMovtRelocation(relocType uint32) bool {
	return relocType == rArmMovwAbsNC || relocType == rArmMovtAbs
}

func decodeArmMovRelocation(file *elf.File, reloc relocation, baseVMA uint64, blob []byte) (armMovRelocation, bool) {
	if reloc.Offset < baseVMA {
		return armMovRelocation{}, false
	}
	blobOffset := reloc.Offset - baseVMA
	if blobOffset+4 > uint64(len(blob)) {
		return armMovRelocation{}, false
	}
	instruction := file.ByteOrder.Uint32(blob[blobOffset : blobOffset+4])
	if !armInstructionMatchesMovRelocation(instruction, reloc.Type) {
		return armMovRelocation{}, false
	}
	return armMovRelocation{
		BlobOffset: blobOffset,
		Type:       reloc.Type,
		Sym:        reloc.Sym,
		Rd:         uint8((instruction >> 12) & 0x0f),
		Imm:        armMovImm16(instruction),
	}, true
}

func armInstructionMatchesMovRelocation(instruction uint32, relocType uint32) bool {
	const armMovOpcodeMask uint32 = 0x0ff00000
	switch relocType {
	case rArmMovwAbsNC:
		return instruction&armMovOpcodeMask == 0x03000000
	case rArmMovtAbs:
		return instruction&armMovOpcodeMask == 0x03400000
	default:
		return false
	}
}

func armMovImm16(instruction uint32) uint16 {
	return uint16(((instruction >> 4) & 0xf000) | (instruction & 0x0fff))
}

func armMovWithImm16(instruction uint32, imm uint16) uint32 {
	instruction &^= 0x000f0fff
	instruction |= (uint32(imm) & 0xf000) << 4
	instruction |= uint32(imm) & 0x0fff
	return instruction
}

func recordArmMovwMovtRelocations(file *elf.File, baseVMA uint64, blob []byte, relocs []armMovRelocation, seen map[uint64]struct{}, offsets *[]uint64) error {
	if file == nil || file.Machine != elf.EM_ARM || len(relocs) == 0 {
		return nil
	}
	usedMovt := make([]bool, len(relocs))
	for i, movw := range relocs {
		if movw.Type != rArmMovwAbsNC {
			continue
		}
		movtIndex := -1
		for j := i + 1; j < len(relocs); j++ {
			movt := relocs[j]
			if movt.Type != rArmMovtAbs || usedMovt[j] || movt.Sym != movw.Sym || movt.Rd != movw.Rd {
				continue
			}
			movtIndex = j
			break
		}
		if movtIndex < 0 {
			continue
		}
		movt := relocs[movtIndex]
		if _, exists := seen[movw.BlobOffset]; exists {
			continue
		}
		if _, exists := seen[movt.BlobOffset]; exists {
			continue
		}
		if movw.BlobOffset > virtuaArmMovRelocMask || movt.BlobOffset > virtuaArmMovRelocMask {
			return fmt.Errorf("ARM MOVW/MOVT relocation offset exceeds encodable range: movw=0x%x movt=0x%x", movw.BlobOffset, movt.BlobOffset)
		}

		absolute := (uint64(movt.Imm) << 16) | uint64(movw.Imm)
		imageRelative := absolute
		if absolute >= baseVMA {
			imageRelative = absolute - baseVMA
		}
		if imageRelative > uint64(^uint32(0)) {
			return fmt.Errorf("ARM MOVW/MOVT relocation value exceeds 32 bits: 0x%x", imageRelative)
		}

		movwInstruction := file.ByteOrder.Uint32(blob[movw.BlobOffset : movw.BlobOffset+4])
		movtInstruction := file.ByteOrder.Uint32(blob[movt.BlobOffset : movt.BlobOffset+4])
		file.ByteOrder.PutUint32(blob[movw.BlobOffset:movw.BlobOffset+4], armMovWithImm16(movwInstruction, uint16(imageRelative&0xffff)))
		file.ByteOrder.PutUint32(blob[movt.BlobOffset:movt.BlobOffset+4], armMovWithImm16(movtInstruction, uint16(imageRelative>>16)))

		usedMovt[movtIndex] = true
		seen[movw.BlobOffset] = struct{}{}
		seen[movt.BlobOffset] = struct{}{}
		*offsets = append(*offsets, packArmMovwMovtRelocation(movw.BlobOffset, movt.BlobOffset))
	}
	return nil
}

func packArmMovwMovtRelocation(movwOffset, movtOffset uint64) uint64 {
	return (virtuaRelocKindArmMovwMovt << virtuaRelocKindShift) |
		((movwOffset & virtuaArmMovRelocMask) << virtuaArmMovRelocBits) |
		(movtOffset & virtuaArmMovRelocMask)
}

func unpackArmMovwMovtRelocation(record uint64) (uint64, uint64) {
	payload := record & virtuaRelocPayloadMask
	return (payload >> virtuaArmMovRelocBits) & virtuaArmMovRelocMask, payload & virtuaArmMovRelocMask
}

func recordArmGOTSlotRelocations(file *elf.File,
	baseVMA uint64,
	blob []byte,
	targets map[uint64]struct{},
	seen map[uint64]struct{},
	offsets *[]uint64) {
	if file == nil || file.Machine != elf.EM_ARM || len(targets) == 0 || len(blob) == 0 {
		return
	}

	imageEnd := baseVMA + uint64(len(blob))
	if imageEnd < baseVMA {
		return
	}
	targetWords := make(map[uint32]struct{}, len(targets))
	for target := range targets {
		if target < baseVMA || target >= imageEnd || target > uint64(^uint32(0)) {
			continue
		}
		targetWords[uint32(target)] = struct{}{}
	}
	if len(targetWords) == 0 {
		return
	}

	// LLD resolves R_ARM_GOT_PREL into a PC-relative displacement to a
	// linker-synthesized GOT slot, but --emit-relocs does not emit a second
	// relocation for the slot itself.  At runtime the instruction still finds
	// the relocated slot (because its displacement is PC-relative), while the
	// slot contains the link-time absolute address.  Locate those synthesized
	// words in writable alloc sections and put them in Virtua's pointer table.
	// Normal R_ARM_ABS32 data pointers have already been recorded in seen, so
	// this only fills the GOT hole rather than heuristically rebasing all data.
	for _, section := range file.Sections {
		if section == nil || section.Type == elf.SHT_NOBITS || section.Size == 0 ||
			section.Flags&elf.SHF_ALLOC == 0 || section.Flags&elf.SHF_WRITE == 0 ||
			section.Addr < baseVMA {
			continue
		}
		start := section.Addr - baseVMA
		if start >= uint64(len(blob)) {
			continue
		}
		end := start + section.Size
		if end < start || end > uint64(len(blob)) {
			end = uint64(len(blob))
		}
		for offset := alignUp(start, 4); offset+4 <= end; offset += 4 {
			if _, exists := seen[offset]; exists {
				continue
			}
			value := file.ByteOrder.Uint32(blob[offset : offset+4])
			if _, wanted := targetWords[value]; !wanted {
				continue
			}
			file.ByteOrder.PutUint32(blob[offset:offset+4], uint32(uint64(value)-baseVMA))
			seen[offset] = struct{}{}
			*offsets = append(*offsets, offset)
		}
	}
}

func recordArmInitArrayPointerRelocations(file *elf.File, baseVMA uint64, blob []byte, seen map[uint64]struct{}, offsets *[]uint64) {
	if file == nil || file.Machine != elf.EM_ARM || pointerSize(file) != 4 {
		return
	}
	imageEnd := baseVMA + uint64(len(blob))
	for _, sectionName := range []string{".preinit_array", ".init_array", ".fini_array"} {
		section := file.Section(sectionName)
		if section == nil || section.Type == elf.SHT_NOBITS || section.Size == 0 || section.Addr < baseVMA {
			continue
		}
		start := section.Addr - baseVMA
		end := start + section.Size
		if end > uint64(len(blob)) {
			end = uint64(len(blob))
		}
		for offset := alignUp(start, 4); offset+4 <= end; offset += 4 {
			if _, exists := seen[offset]; exists {
				continue
			}
			value := uint64(file.ByteOrder.Uint32(blob[offset : offset+4]))
			if value == 0 || value < baseVMA || value >= imageEnd {
				continue
			}
			file.ByteOrder.PutUint32(blob[offset:offset+4], uint32(value-baseVMA))
			seen[offset] = struct{}{}
			*offsets = append(*offsets, offset)
		}
	}
}

func recordGoPclnTextStartRelocation(file *elf.File, baseVMA uint64, blob []byte, seen map[uint64]struct{}, offsets *[]uint64) {
	if pointerSize(file) != 8 {
		return
	}
	section := file.Section(".gopclntab")
	if section == nil || section.Type == elf.SHT_NOBITS || section.Size < 32 || section.Addr < baseVMA {
		return
	}
	start := section.Addr - baseVMA
	if start+32 > uint64(len(blob)) {
		return
	}
	if file.ByteOrder.Uint32(blob[start:start+4]) != 0xfffffff1 {
		return
	}

	const textStartOffset64 = 24
	offset := start + textStartOffset64
	if offset+8 > uint64(len(blob)) {
		return
	}
	if _, exists := seen[offset]; exists {
		return
	}
	value := file.ByteOrder.Uint64(blob[offset : offset+8])
	if value < baseVMA || value >= baseVMA+uint64(len(blob)) {
		return
	}

	binary.LittleEndian.PutUint64(blob[offset:offset+8], value-baseVMA)
	seen[offset] = struct{}{}
	*offsets = append(*offsets, offset)
}

func recordStaticDataPointerRelocations(file *elf.File, baseVMA uint64, blob []byte, seen map[uint64]struct{}, offsets *[]uint64) {
	ptrSize := pointerSize(file)
	imageEnd := baseVMA + uint64(len(blob))
	scanGoReadOnly := isGoImage(file)
	for _, section := range file.Sections {
		if section.Type == elf.SHT_NOBITS || section.Size == 0 {
			continue
		}
		if section.Flags&elf.SHF_ALLOC == 0 || section.Flags&elf.SHF_WRITE == 0 {
			if !scanGoReadOnly || section.Flags&elf.SHF_ALLOC == 0 || section.Flags&elf.SHF_EXECINSTR != 0 || section.Name == ".gopclntab" {
				continue
			}
		}
		if section.Addr < baseVMA || section.Addr >= imageEnd {
			continue
		}

		start := section.Addr - baseVMA
		end := start + section.Size
		if end > uint64(len(blob)) {
			end = uint64(len(blob))
		}
		for offset := alignUp(start, ptrSize); offset+ptrSize <= end; offset += ptrSize {
			if _, exists := seen[offset]; exists {
				continue
			}
			value := readPointer(file, blob[offset:offset+ptrSize])
			if value == 0 {
				continue
			}
			if value < baseVMA || value > imageEnd {
				continue
			}
			writePointer(file, blob[offset:offset+ptrSize], value-baseVMA)
			seen[offset] = struct{}{}
			*offsets = append(*offsets, offset)
		}
	}
}

func isGoImage(file *elf.File) bool {
	return file.Section(".gopclntab") != nil || file.Section(".note.go.buildid") != nil
}

func recordX8664GOT64SlotRelocation(file *elf.File, reloc relocation, baseVMA uint64, blob []byte, syms []elf.Symbol, seen map[uint64]struct{}, offsets *[]uint64) (bool, error) {
	if reloc.Offset < baseVMA {
		return false, nil
	}

	fieldOffset := reloc.Offset - baseVMA
	if fieldOffset+8 > uint64(len(blob)) {
		return false, nil
	}

	gotBase, ok := findSymbolValue(file, "_GLOBAL_OFFSET_TABLE_")
	if !ok || gotBase < baseVMA {
		return false, nil
	}
	gotSlotVMA := int64(gotBase) + int64(file.ByteOrder.Uint64(blob[fieldOffset:fieldOffset+8]))
	if gotSlotVMA < int64(baseVMA) {
		return false, nil
	}
	gotOffset := uint64(gotSlotVMA) - baseVMA
	if gotOffset+8 > uint64(len(blob)) {
		return false, nil
	}
	if target := sectionByVMA(file, uint64(gotSlotVMA)); target == nil || target.Flags&elf.SHF_WRITE == 0 {
		return false, nil
	}
	if _, exists := seen[gotOffset]; exists {
		return true, nil
	}

	slotStart := int(gotOffset)
	slotEnd := slotStart + 8
	absValue := file.ByteOrder.Uint64(blob[slotStart:slotEnd])
	if absValue < baseVMA || absValue >= baseVMA+uint64(len(blob)) {
		symValue, undefined, ok := relocationSymbolValue(reloc, syms)
		if !ok || undefined {
			return false, nil
		}
		absValue = symValue + uint64(reloc.Addend)
	}
	if absValue == 0 {
		return false, nil
	}
	imageRelative := absValue
	if absValue >= baseVMA {
		imageRelative = absValue - baseVMA
	}

	binary.LittleEndian.PutUint64(blob[slotStart:slotEnd], imageRelative)
	seen[gotOffset] = struct{}{}
	*offsets = append(*offsets, gotOffset)
	return true, nil
}

func recordX8664GOTSlotRelocation(file *elf.File, reloc relocation, baseVMA uint64, blob []byte, syms []elf.Symbol, seen map[uint64]struct{}, offsets *[]uint64) (bool, error) {
	if reloc.Offset < baseVMA {
		return false, nil
	}

	fieldOffset := reloc.Offset - baseVMA
	if fieldOffset+4 > uint64(len(blob)) {
		return false, nil
	}

	disp := int64(int32(file.ByteOrder.Uint32(blob[fieldOffset : fieldOffset+4])))
	gotVMA := int64(reloc.Offset) + 4 + disp
	if gotVMA < int64(baseVMA) {
		return false, nil
	}

	gotOffset := uint64(gotVMA) - baseVMA
	if gotOffset+8 > uint64(len(blob)) {
		return false, nil
	}
	if target := sectionByVMA(file, uint64(gotVMA)); target == nil || target.Flags&elf.SHF_WRITE == 0 {
		return false, nil
	}
	if _, exists := seen[gotOffset]; exists {
		return true, nil
	}

	slotStart := int(gotOffset)
	slotEnd := slotStart + 8
	existing := file.ByteOrder.Uint64(blob[slotStart:slotEnd])
	absValue := existing
	if absValue < baseVMA || absValue >= baseVMA+uint64(len(blob)) {
		var ok bool
		absValue, ok = relocationGOTTargetValue(reloc, syms)
		if !ok {
			return false, nil
		}
	}
	if absValue == 0 {
		return false, nil
	}
	imageRelative := absValue
	if absValue >= baseVMA {
		imageRelative = absValue - baseVMA
	}

	binary.LittleEndian.PutUint64(blob[slotStart:slotEnd], imageRelative)
	seen[gotOffset] = struct{}{}
	*offsets = append(*offsets, gotOffset)
	return true, nil
}

func relocationGOTTargetValue(reloc relocation, syms []elf.Symbol) (uint64, bool) {
	symValue, undefined, ok := relocationSymbolValue(reloc, syms)
	if !ok || undefined {
		return 0, ok
	}

	// x86-64 GOTPCREL relocations address the GOT slot through a RIP-relative
	// 32-bit displacement. The retained relocation addend normally includes the
	// -4 RIP compensation, so undo that before using it as a target addend.
	targetAddend := reloc.Addend + 4
	if targetAddend < 0 {
		negative := uint64(-targetAddend)
		if negative > symValue {
			return 0, false
		}
		return symValue - negative, true
	}
	addend := uint64(targetAddend)
	if ^uint64(0)-symValue < addend {
		return 0, false
	}
	return symValue + addend, true
}

func relocationAbsoluteValue(file *elf.File, reloc relocation, existing uint64, syms []elf.Symbol) (uint64, bool) {
	switch file.Machine {
	case elf.EM_RISCV:
		switch reloc.Type {
		case rRiscvRelative:
			if reloc.Addend != 0 {
				return uint64(reloc.Addend), true
			}
			return existing, true
		case rRiscv64:
			symValue, undefined, ok := relocationSymbolValue(reloc, syms)
			if !ok {
				return 0, false
			}
			if undefined {
				return 0, true
			}
			return symValue + uint64(reloc.Addend), true
		}
	case elf.EM_X86_64:
		switch reloc.Type {
		case rX8664Relative:
			if reloc.Addend != 0 {
				return uint64(reloc.Addend), true
			}
			return existing, true
		case rX8664Abs64, rX8664GlobDat, rX8664JumpSlot:
			symValue, undefined, ok := relocationSymbolValue(reloc, syms)
			if !ok {
				return 0, false
			}
			if undefined {
				return 0, true
			}
			return symValue + uint64(reloc.Addend), true
		}
	case elf.EM_AARCH64:
		switch reloc.Type {
		case rAarch64Relative:
			if reloc.Addend != 0 {
				return uint64(reloc.Addend), true
			}
			return existing, true
		case rAarch64Abs64, rAarch64GlobDat, rAarch64JumpSlot:
			symValue, undefined, ok := relocationSymbolValue(reloc, syms)
			if !ok {
				return 0, false
			}
			if undefined {
				return 0, true
			}
			return symValue + uint64(reloc.Addend), true
		}
	case elf.EM_ARM:
		switch reloc.Type {
		case rArmRelative:
			if reloc.Addend != 0 {
				return uint64(reloc.Addend), true
			}
			return existing, true
		case rArmAbs32, rArmTarget1, rArmGlobDat, rArmJumpSlot:
			symValue, undefined, ok := relocationSymbolValue(reloc, syms)
			if !ok {
				return 0, false
			}
			if undefined {
				return 0, true
			}
			if reloc.Addend != 0 {
				return symValue + uint64(reloc.Addend), true
			}
			if existing != 0 {
				return existing, true
			}
			return symValue, true
		}
	}
	return 0, false
}

func relocationSymbolValue(reloc relocation, syms []elf.Symbol) (uint64, bool, bool) {
	if reloc.Sym == 0 {
		return 0, false, true
	}

	idx := int(reloc.Sym)
	if idx <= 0 || idx > len(syms) {
		return 0, false, false
	}

	sym := syms[idx-1]
	if sym.Value == 0 && sym.Section == elf.SHN_UNDEF {
		return 0, true, true
	}
	return sym.Value, false, true
}

func shouldProcessRelocationSection(file *elf.File, section *elf.Section) bool {
	switch section.Name {
	case ".rela", ".rel",
		".rela.dyn", ".rela.got", ".rela.plt", ".rel.dyn", ".rel.got", ".rel.plt":
		// Dynamic relocation sections, including the bare ".rela" Go's internal
		// linker emits for -buildmode=pie. Processing these gives the loader a
		// precise list of pointers to rebase, which Go images need: the
		// heuristic read-only pointer scan otherwise misreads stack maps in
		// .rodata as pointers and corrupts them (copystack then reports
		// "invalid pointer found on stack").
		return true
	}

	target := sectionByELFIndex(file, section.SectionHeader.Info)
	if target == nil {
		return false
	}

	return target.Flags&elf.SHF_ALLOC != 0
}

func isX8664GOTPCRELRelocation(relocType uint32) bool {
	return relocType == rX8664GOTPCREL ||
		relocType == rX8664GOTPCRELX ||
		relocType == rX8664RexGOTPCRELX
}

func symbolsForRelocationSection(file *elf.File, section *elf.Section) ([]elf.Symbol, error) {
	linked := sectionByELFIndex(file, section.SectionHeader.Link)
	if linked != nil {
		switch linked.Type {
		case elf.SHT_SYMTAB:
			return file.Symbols()
		case elf.SHT_DYNSYM:
			return file.DynamicSymbols()
		}
	}
	if syms, err := file.Symbols(); err == nil {
		return syms, nil
	}
	return file.DynamicSymbols()
}

func sectionByELFIndex(file *elf.File, index uint32) *elf.Section {
	if file == nil || index == 0 {
		return nil
	}

	// sh_info/sh_link keep the original ELF section indices. debug/elf has
	// included the null section in some Go versions and omitted it in others, so
	// map both shapes onto the same ELF index space.
	sliceIndex := int(index)
	if len(file.Sections) == 0 || file.Sections[0].Type != elf.SHT_NULL || file.Sections[0].Name != "" {
		sliceIndex--
	}
	if sliceIndex < 0 || sliceIndex >= len(file.Sections) {
		return nil
	}
	return file.Sections[sliceIndex]
}

func sectionByVMA(file *elf.File, vma uint64) *elf.Section {
	if file == nil {
		return nil
	}
	for _, section := range file.Sections {
		if section.Flags&elf.SHF_ALLOC == 0 || section.Size == 0 {
			continue
		}
		if vma >= section.Addr && vma < section.Addr+section.Size {
			return section
		}
	}
	return nil
}

func decodeRelocations(file *elf.File, section *elf.Section) ([]relocation, error) {
	data, err := sectionData(section)
	if err != nil {
		return nil, err
	}

	entrySize := int(section.Entsize)
	if entrySize == 0 {
		if file.Class == elf.ELFCLASS32 && section.Type == elf.SHT_RELA {
			entrySize = 12
		} else if file.Class == elf.ELFCLASS32 {
			entrySize = 8
		} else if section.Type == elf.SHT_RELA {
			entrySize = 24
		} else {
			entrySize = 16
		}
	}
	if len(data)%entrySize != 0 {
		return nil, fmt.Errorf("malformed relocation payload in %s", section.Name)
	}

	relocations := make([]relocation, 0, len(data)/entrySize)
	for offset := 0; offset+entrySize <= len(data); offset += entrySize {
		var rOffset uint64
		var rInfo uint64
		if file.Class == elf.ELFCLASS32 {
			rOffset = uint64(file.ByteOrder.Uint32(data[offset : offset+4]))
			rInfo = uint64(file.ByteOrder.Uint32(data[offset+4 : offset+8]))
		} else {
			rOffset = file.ByteOrder.Uint64(data[offset : offset+8])
			rInfo = file.ByteOrder.Uint64(data[offset+8 : offset+16])
		}

		reloc := relocation{
			Offset: rOffset,
		}
		if file.Class == elf.ELFCLASS32 {
			reloc.Type = uint32(rInfo & 0xff)
			reloc.Sym = uint32(rInfo >> 8)
		} else {
			reloc.Type = uint32(rInfo & 0xffffffff)
			reloc.Sym = uint32(rInfo >> 32)
		}
		if section.Type == elf.SHT_RELA {
			if file.Class == elf.ELFCLASS32 {
				reloc.Addend = int64(int32(file.ByteOrder.Uint32(data[offset+8 : offset+12])))
			} else {
				reloc.Addend = int64(file.ByteOrder.Uint64(data[offset+16 : offset+24]))
			}
		}
		relocations = append(relocations, reloc)
	}
	return relocations, nil
}

func sectionData(section *elf.Section) ([]byte, error) {
	if section.ReaderAt != nil && section.Size != 0 {
		data := make([]byte, section.Size)
		if _, err := section.ReaderAt.ReadAt(data, 0); err != nil && err != io.EOF {
			return nil, err
		}
		return data, nil
	}
	return section.Data()
}

func isSupportedRelocation(machine elf.Machine, relocType uint32) bool {
	switch machine {
	case elf.EM_RISCV:
		return relocType == rRiscv64 || relocType == rRiscvRelative
	case elf.EM_X86_64:
		return relocType == rX8664Abs64 ||
			relocType == rX8664GlobDat ||
			relocType == rX8664JumpSlot ||
			relocType == rX8664Relative
	case elf.EM_AARCH64:
		return relocType == rAarch64Abs64 ||
			relocType == rAarch64GlobDat ||
			relocType == rAarch64JumpSlot ||
			relocType == rAarch64Relative
	case elf.EM_ARM:
		return relocType == rArmAbs32 ||
			relocType == rArmTarget1 ||
			relocType == rArmGlobDat ||
			relocType == rArmJumpSlot ||
			relocType == rArmRelative
	default:
		return false
	}
}

func supportsMachine(machine elf.Machine) bool {
	return machine == elf.EM_RISCV || machine == elf.EM_X86_64 || machine == elf.EM_AARCH64 || machine == elf.EM_ARM
}

func requiresGlobalPointer(machine elf.Machine) bool {
	return machine == elf.EM_RISCV
}

func pointerSize(file *elf.File) uint64 {
	if file != nil && file.Class == elf.ELFCLASS32 {
		return 4
	}
	return 8
}

func readPointer(file *elf.File, data []byte) uint64 {
	if pointerSize(file) == 4 {
		return uint64(file.ByteOrder.Uint32(data[:4]))
	}
	return file.ByteOrder.Uint64(data[:8])
}

func writePointer(file *elf.File, data []byte, value uint64) {
	if pointerSize(file) == 4 {
		binary.LittleEndian.PutUint32(data[:4], uint32(value))
		return
	}
	binary.LittleEndian.PutUint64(data[:8], value)
}

func alignUp(value, alignment uint64) uint64 {
	if alignment == 0 {
		return value
	}
	return (value + alignment - 1) &^ (alignment - 1)
}

func archFlag(machine elf.Machine) uint64 {
	switch machine {
	case elf.EM_X86_64:
		return virtuaFlagArchX8664
	case elf.EM_RISCV:
		return virtuaFlagArchRiscv64
	case elf.EM_AARCH64:
		return virtuaFlagArchAarch64
	case elf.EM_ARM:
		return virtuaFlagArchArm32
	default:
		return 0
	}
}

func findGlobalPointer(file *elf.File, baseVMA uint64) uint64 {
	if gp, ok := findSymbolValue(file, "__global_pointer$"); ok {
		return toOffset(gp, baseVMA)
	}

	// GNU ld's RISC-V default linker script defines roughly:
	//   __global_pointer$ = MIN(__SDATA_BEGIN__ + 0x800,
	//       MAX(__DATA_BEGIN__ + 0x800, __BSS_END__ - 0x800))
	// If a stripped binary lost the symbol, infer that value from sections.
	if sdata := firstSectionAddr(file, ".sdata", ".srodata"); sdata != 0 {
		dataBegin := firstSectionAddr(file, ".data", ".sdata", ".got")
		if dataBegin == 0 {
			dataBegin = sdata
		}
		bssEnd := maxSectionEnd(file, ".sbss", ".bss", ".scommon")
		lo := dataBegin + 0x800
		hi := uint64(0)
		if bssEnd > 0x800 {
			hi = bssEnd - 0x800
		}
		gp := max64(lo, hi)
		gp = min64(sdata+0x800, gp)
		return toOffset(gp, baseVMA)
	}

	if got := firstSectionAddr(file, ".got", ".got.plt"); got != 0 {
		return toOffset(got+0x800, baseVMA)
	}
	if data := firstSectionAddr(file, ".data"); data != 0 {
		return toOffset(data+0x800, baseVMA)
	}

	return 0
}

func findSymbolValue(file *elf.File, name string) (uint64, bool) {
	if symbols, err := file.Symbols(); err == nil {
		for _, symbol := range symbols {
			if symbol.Name == name && symbol.Value != 0 {
				return symbol.Value, true
			}
		}
	}
	if symbols, err := file.DynamicSymbols(); err == nil {
		for _, symbol := range symbols {
			if symbol.Name == name && symbol.Value != 0 {
				return symbol.Value, true
			}
		}
	}
	return 0, false
}

func firstSectionAddr(file *elf.File, names ...string) uint64 {
	for _, name := range names {
		if section := file.Section(name); section != nil && section.Flags&elf.SHF_ALLOC != 0 && section.Size != 0 {
			return section.Addr
		}
	}
	return 0
}

func maxSectionEnd(file *elf.File, names ...string) uint64 {
	var end uint64
	for _, name := range names {
		if section := file.Section(name); section != nil && section.Flags&elf.SHF_ALLOC != 0 && section.Size != 0 {
			if section.Addr+section.Size > end {
				end = section.Addr + section.Size
			}
		}
	}
	return end
}

func toOffset(value, baseVMA uint64) uint64 {
	if value >= baseVMA {
		return value - baseVMA
	}
	return value
}

func min64(a, b uint64) uint64 {
	if a < b {
		return a
	}
	return b
}

func max64(a, b uint64) uint64 {
	if a > b {
		return a
	}
	return b
}

func findSectionSize(file *elf.File, name string) uint64 {
	section := file.Section(name)
	if section == nil {
		return 0
	}
	return section.Size
}
