// Writes the decompiled functions that use each given string (or, for "@<address>", that call
// the function at that address; for "!<address>", the function itself, made one if the program
// was not analysed), one file per argument, to the output folder: for tracing a
// game's logic from the names in its data and messages.
//   analyzeHeadless <project dir> <name> -process <program> -noanalysis \
//       -scriptPath tools/ghidra -postScript ExportUsers.java <output dir> <string>...
// @category ps-river

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Reference;

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashSet;
import java.util.Set;

public class ExportUsers extends GhidraScript {
    private static final int MaxPlaces = 16;
    private static final int TimeoutSeconds = 60;

    @Override
    protected void run() throws Exception {
        final String[] args = getScriptArgs();
        final File out = new File(args[0]);
        out.mkdirs();
        final DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        final Memory memory = currentProgram.getMemory();
        for (int i = 1; i < args.length; ++i) {
            final String text = args[i];
            if (text.startsWith("@")) {
                exportCallers(out, decompiler, text.substring(1));
                continue;
            }
            if (text.startsWith("!")) {
                exportFunction(out, decompiler, text.substring(1));
                continue;
            }
            final byte[] needle = (text + "\0").getBytes(StandardCharsets.US_ASCII);
            final Set<Function> users = new LinkedHashSet<>();
            final StringBuilder places = new StringBuilder();
            Address from = memory.getMinAddress();
            for (int found = 0; found < MaxPlaces; ++found) {
                final Address at = memory.findBytes(from, needle, null, true, monitor);
                if (at == null) {
                    break;
                }
                places.append("string at ").append(at).append('\n');
                for (final Reference ref : getReferencesTo(at)) {
                    final Function f = getFunctionContaining(ref.getFromAddress());
                    places.append("  used at ").append(ref.getFromAddress())
                          .append(f != null ? " in " + f.getEntryPoint() : " (no function)").append('\n');
                    if (f != null) {
                        users.add(f);
                    }
                }
                from = at.add(1);
            }
            final String name = text.replaceAll("[^A-Za-z0-9_]", "_");
            try (PrintWriter w = new PrintWriter(new File(out, name + ".c"), StandardCharsets.UTF_8)) {
                w.println("// " + text);
                w.print(places);
                for (final Function f : users) {
                    final DecompileResults r = decompiler.decompileFunction(f, TimeoutSeconds, monitor);
                    w.println("\n// ---- " + f.getEntryPoint());
                    w.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// (did not decompile)");
                }
            }
            println(text + ": " + users.size() + " functions");
        }
        decompiler.dispose();
    }

    private void exportFunction(File out, DecompInterface decompiler, String address) throws Exception {
        final Address entry = toAddr(Long.decode(address));
        Function f = getFunctionContaining(entry);
        if (f == null) {
            disassemble(entry);
            f = createFunction(entry, null);
        }
        try (PrintWriter w = new PrintWriter(new File(out, "function_" + address + ".c"), StandardCharsets.UTF_8)) {
            if (f == null) {
                w.println("// no function at " + address);
                return;
            }
            final DecompileResults r = decompiler.decompileFunction(f, TimeoutSeconds, monitor);
            w.println("// ---- " + f.getEntryPoint());
            w.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// (did not decompile)");
        }
    }

    private void exportCallers(File out, DecompInterface decompiler, String address) throws Exception {
        final Address entry = toAddr(Long.decode(address));
        final Set<Function> callers = new LinkedHashSet<>();
        final StringBuilder places = new StringBuilder();
        for (final Reference ref : getReferencesTo(entry)) {
            final Function f = getFunctionContaining(ref.getFromAddress());
            places.append("called at ").append(ref.getFromAddress())
                  .append(f != null ? " in " + f.getEntryPoint() : " (no function)").append('\n');
            if (f != null) {
                callers.add(f);
            }
        }
        try (PrintWriter w = new PrintWriter(new File(out, "callers_" + address + ".c"), StandardCharsets.UTF_8)) {
            w.println("// callers of " + address);
            w.print(places);
            for (final Function f : callers) {
                final DecompileResults r = decompiler.decompileFunction(f, TimeoutSeconds, monitor);
                w.println("\n// ---- " + f.getEntryPoint());
                w.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// (did not decompile)");
            }
        }
        println("callers of " + address + ": " + callers.size() + " functions");
    }
}
