import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

public class ExportGameAnalysis extends GhidraScript {
    private static String quote(String value) {
        if (value == null) return "null";
        return "\"" + value.replace("\\", "\\\\").replace("\"", "\\\"") + "\"";
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 4) {
            throw new IllegalArgumentException("usage: OUT.json ENTRY LO HI_EXCLUSIVE");
        }
        Path output = Path.of(args[0]);
        long entryValue = Long.decode(args[1]);
        long lo = Long.decode(args[2]);
        long hi = Long.decode(args[3]);
        Address entry = toAddr(entryValue);
        Listing listing = currentProgram.getListing();
        Function entryFunction = currentProgram.getFunctionManager().getFunctionAt(entry);

        List<String> blocks = new ArrayList<>();
        for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
            blocks.add("{" +
                "\"name\":" + quote(block.getName()) + "," +
                "\"start\":" + quote(block.getStart().toString()) + "," +
                "\"end\":" + quote(block.getEnd().toString()) + "," +
                "\"size\":" + block.getSize() +
                "}");
        }

        List<String> functions = new ArrayList<>();
        List<String> excludedFunctions = new ArrayList<>();
        FunctionIterator iterator = currentProgram.getFunctionManager().getFunctions(true);
        while (iterator.hasNext()) {
            Function function = iterator.next();
            long address = function.getEntryPoint().getOffset();
            if (address < lo || address >= hi || (address & 3L) != 0L) continue;
            Instruction instruction = listing.getInstructionAt(function.getEntryPoint());
            if (instruction == null) continue;

            // Ghidra can create a one-instruction function when packed data
            // happens to decode as JAL. Reject only the narrow, provable case:
            // the instruction falls through, its entire function body is that
            // one instruction, and every reference comes from outside any
            // function. Keep the required executable entry unconditionally.
            List<String> referenceSources = new ArrayList<>();
            boolean referencedFromFunction = false;
            ReferenceIterator references = currentProgram.getReferenceManager()
                .getReferencesTo(function.getEntryPoint());
            while (references.hasNext()) {
                Reference reference = references.next();
                referenceSources.add(quote(reference.getFromAddress().toString()));
                if (currentProgram.getFunctionManager()
                        .getFunctionContaining(reference.getFromAddress()) != null) {
                    referencedFromFunction = true;
                }
            }
            boolean dataDerivedFalseFunction = address != entryValue &&
                function.getBody().getNumAddresses() == instruction.getLength() &&
                instruction.getFallThrough() != null &&
                !referenceSources.isEmpty() &&
                !referencedFromFunction;
            if (dataDerivedFalseFunction) {
                excludedFunctions.add("{" +
                    "\"entry\":" + quote(String.format("0x%08X", address)) + "," +
                    "\"name\":" + quote(function.getName()) + "," +
                    "\"instruction\":" + quote(instruction.toString()) + "," +
                    "\"reason\":" + quote("single fallthrough instruction referenced only from outside functions") + "," +
                    "\"reference_sources\":[" + String.join(",", referenceSources) + "]" +
                    "}");
                continue;
            }
            functions.add("{" +
                "\"entry\":" + quote(String.format("0x%08X", address)) + "," +
                "\"name\":" + quote(function.getName()) + "," +
                "\"body_min\":" + quote(function.getBody().getMinAddress().toString()) + "," +
                "\"body_max\":" + quote(function.getBody().getMaxAddress().toString()) + "," +
                "\"instruction\":" + quote(instruction.toString()) +
                "}");
        }

        List<String> entryInstructions = new ArrayList<>();
        Instruction instruction = listing.getInstructionAt(entry);
        for (int index = 0; index < 8 && instruction != null; ++index) {
            byte[] bytes = instruction.getBytes();
            StringBuilder word = new StringBuilder();
            for (int byteIndex = bytes.length - 1; byteIndex >= 0; --byteIndex) {
                word.append(String.format("%02X", bytes[byteIndex] & 0xff));
            }
            entryInstructions.add("{" +
                "\"address\":" + quote(instruction.getAddress().toString()) + "," +
                "\"word_little_endian\":" + quote("0x" + word) + "," +
                "\"disassembly\":" + quote(instruction.toString()) +
                "}");
            instruction = instruction.getNext();
        }

        String json = "{\n" +
            "  \"schema\": 1,\n" +
            "  \"program\": " + quote(currentProgram.getName()) + ",\n" +
            "  \"language\": " + quote(currentProgram.getLanguageID().toString()) + ",\n" +
            "  \"image_base\": " + quote(currentProgram.getImageBase().toString()) + ",\n" +
            "  \"entry\": " + quote(String.format("0x%08X", entryValue)) + ",\n" +
            "  \"entry_function\": " + quote(entryFunction == null ? null : entryFunction.getName()) + ",\n" +
            "  \"memory_blocks\": [" + String.join(",", blocks) + "],\n" +
            "  \"entry_instructions\": [" + String.join(",", entryInstructions) + "],\n" +
            "  \"function_count\": " + functions.size() + ",\n" +
            "  \"excluded_function_count\": " + excludedFunctions.size() + ",\n" +
            "  \"excluded_functions\": [" + String.join(",", excludedFunctions) + "],\n" +
            "  \"functions\": [\n    " + String.join(",\n    ", functions) + "\n  ]\n" +
            "}\n";
        Files.createDirectories(output.getParent());
        Files.writeString(output, json, StandardCharsets.UTF_8);
        println("wrote " + output + " (" + functions.size() + " functions, " +
            excludedFunctions.size() + " excluded data-derived functions)");
    }
}
