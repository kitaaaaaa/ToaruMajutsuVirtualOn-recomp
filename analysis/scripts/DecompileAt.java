// Decompiles the function containing each address given as an argument (image offsets, hex).
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompileAt extends GhidraScript {
	@Override
	protected void run() throws Exception {
		DecompInterface d = new DecompInterface();
		d.openProgram(currentProgram);
		long base = currentProgram.getImageBase().getOffset();
		for (String arg : getScriptArgs()) {
			Address a = toAddr(base + Long.decode(arg));
			Function f = getFunctionContaining(a);
			if (f == null) {
				println("DECOMP none at " + a);
				continue;
			}
			DecompileResults r = d.decompileFunction(f, 120, monitor);
			println("DECOMP ==== " + a + " in " + f.getName() + " at " + f.getEntryPoint());
			if (r.getDecompiledFunction() != null) {
				for (String line : r.getDecompiledFunction().getC().split("\n")) {
					println("DECOMP " + line);
				}
			}
		}
		d.dispose();
	}
}
