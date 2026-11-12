// Decompile a list of functions (by symbol name or 0xADDRESS) to text files, without needing full auto-analysis.
// For each requested entry: disassemble + create the function if missing, create functions for its direct
// call targets (so the decompiler shows names), then write <outdir>/<name>.c.
//
// Usage (headless, on a project imported with -noanalysis or analysed):
//   analyzeHeadless <projdir> <proj> -process <program> -noanalysis -scriptPath <this dir> \
//       -postScript DecompileList.java <outdir> <listfile> [depth]
// listfile: one symbol name or 0x-address per line ('#' comments). depth (default 0): also decompile callees
// up to that depth (only named, non-external ones).
//@category Zombiewood
import java.io.File;
import java.io.PrintWriter;
import java.nio.file.Files;
import java.util.ArrayDeque;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class DecompileList extends GhidraScript {

    private Function ensureFunction(Address a) throws Exception {
        Function f = getFunctionAt(a);
        if (f != null) {
            return f;
        }
        if (getInstructionAt(a) == null) {
            DisassembleCommand dc = new DisassembleCommand(a, null, true);
            dc.applyTo(currentProgram, monitor);
        }
        if (getInstructionAt(a) == null) {
            return null;
        }
        CreateFunctionCmd cf = new CreateFunctionCmd(a);
        cf.applyTo(currentProgram, monitor);
        return getFunctionAt(a);
    }

    private Address resolve(String name) {
        if (name.startsWith("0x")) {
            return toAddr(Long.parseLong(name.substring(2), 16) & ~1L);
        }
        SymbolIterator it = currentProgram.getSymbolTable().getSymbols(name);
        while (it.hasNext()) {
            Symbol s = it.next();
            if (!s.isExternal() && s.getAddress().isMemoryAddress()) {
                return s.getAddress();
            }
        }
        return null;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = new File(args[0]);
        outDir.mkdirs();
        List<String> names = Files.readAllLines(new File(args[1]).toPath());
        int maxDepth = args.length > 2 ? Integer.parseInt(args[2]) : 0;

        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);

        ArrayDeque<Object[]> queue = new ArrayDeque<>();
        Set<Address> done = new HashSet<>();
        for (String n : names) {
            n = n.trim();
            if (n.isEmpty() || n.startsWith("#")) {
                continue;
            }
            Address a = resolve(n);
            if (a == null) {
                println("NOT FOUND: " + n);
                continue;
            }
            queue.add(new Object[] { a, 0, n });
        }
        while (!queue.isEmpty() && !monitor.isCancelled()) {
            Object[] item = queue.poll();
            Address a = (Address) item[0];
            int depth = (Integer) item[1];
            if (!done.add(a)) {
                continue;
            }
            Function f = ensureFunction(a);
            if (f == null) {
                println("NO CODE: " + item[2]);
                continue;
            }
            // make direct call/jump targets functions so they print by name
            AddressSet body = new AddressSet(f.getBody());
            for (Instruction ins : currentProgram.getListing().getInstructions(body, true)) {
                for (Reference r : ins.getReferencesFrom()) {
                    if (!r.getReferenceType().isCall() && !r.getReferenceType().isJump()) {
                        continue;
                    }
                    Address t = r.getToAddress();
                    if (!t.isMemoryAddress() || body.contains(t)) {
                        continue;
                    }
                    Function tf = ensureFunction(t);
                    if (tf != null && depth < maxDepth && !tf.isThunk()) {
                        queue.add(new Object[] { t, depth + 1, tf.getName() });
                    }
                }
            }
            DecompileResults res = dec.decompileFunction(f, 120, monitor);
            String name = f.getName().replaceAll("[^A-Za-z0-9_.$-]", "_");
            if (name.length() > 150) {
                name = name.substring(0, 150);
            }
            File out = new File(outDir, name + "@" + a.toString() + ".c");
            try (PrintWriter pw = new PrintWriter(out, "UTF-8")) {
                pw.println("// " + f.getName(true) + " @ " + a + " size=" + f.getBody().getNumAddresses());
                if (res != null && res.decompileCompleted()) {
                    pw.println(res.getDecompiledFunction().getC());
                } else {
                    pw.println("// DECOMPILE FAILED: " + (res == null ? "null" : res.getErrorMessage()));
                }
            }
        }
        dec.dispose();
    }
}
