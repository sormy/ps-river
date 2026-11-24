// Pre-analysis setup for a raw memory dump of a game's code: no headers or entry points, so
// functions are found by their prologues and from the addresses given.
//   analyzeHeadless ... -import dump.bin -loader BinaryLoader -loader-baseAddr 0x800000000 \
//       -preScript RawSetup.java <function address>...
// @category ps-river

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;

public class RawSetup extends GhidraScript {
    @Override
    protected void run() throws Exception {
        setAnalysisOption(currentProgram, "Aggressive Instruction Finder", "true");
        for (final String text : getScriptArgs()) {
            final Address at = toAddr(Long.decode(text));
            disassemble(at);
            createFunction(at, null);
        }
    }
}
