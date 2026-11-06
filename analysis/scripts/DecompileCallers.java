// Decompiles every function that calls each named function or address (args: symbol names or hex offsets).
import java.util.LinkedHashSet;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.Reference;

public class DecompileCallers extends GhidraScript {
	@Override
	protected void run() throws Exception {
		DecompInterface d = new DecompInterface();
		d.openProgram(currentProgram);
		long base = currentProgram.getImageBase().getOffset();
		Set<Function> done = new LinkedHashSet<>();
		for (String arg : getScriptArgs()) {
			Set<Address> targets = new LinkedHashSet<>();
			if (arg.startsWith("0x")) {
				targets.add(toAddr(base + Long.decode(arg)));
			}
			else {
				FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
				while (it.hasNext()) {
					Function f = it.next();
					if (f.getName().equals(arg)) {
						targets.add(f.getEntryPoint());
					}
				}
			}
			for (Address t : targets) {
				for (Reference r : getReferencesTo(t)) {
					Function caller = getFunctionContaining(r.getFromAddress());
					println("DECOMP -- " + arg + " called from " + r.getFromAddress() + " in " + (caller == null ? "?" : caller.getName()));
					if (caller == null || !done.add(caller)) {
						continue;
					}
					DecompileResults res = d.decompileFunction(caller, 120, monitor);
					if (res.getDecompiledFunction() != null) {
						println("DECOMP ==== " + caller.getName());
						for (String line : res.getDecompiledFunction().getC().split("\n")) {
							println("DECOMP " + line);
						}
					}
				}
			}
		}
		d.dispose();
	}
}
