import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class SeedPsxEntry extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 1) {
            throw new IllegalArgumentException("usage: SeedPsxEntry.java <entry-address>");
        }
        Address entry = toAddr(Long.decode(args[0]));
        disassemble(entry);
        Function function = getFunctionAt(entry);
        if (function == null) {
            function = createFunction(entry, "entry");
        } else {
            function.setName("entry", ghidra.program.model.symbol.SourceType.USER_DEFINED);
        }
        createLabel(entry, "entry", true);
        currentProgram.getSymbolTable().addExternalEntryPoint(entry);
        println("PS-X entry seeded at " + entry);
    }
}
