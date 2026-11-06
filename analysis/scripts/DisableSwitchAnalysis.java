// Pre-script: turns off Ghidra's switch recovery, which over-reads this game's jump tables, and its
// non-return discovery, which marks 013734E0 (it returns through a tail call) and cuts off its callers.
import ghidra.app.script.GhidraScript;

public class DisableSwitchAnalysis extends GhidraScript {
	@Override
	protected void run() throws Exception {
		for (String name : new String[] { "Decompiler Switch Analysis", "Non-Returning Functions - Discovered" }) {
			if (!getCurrentAnalysisOptionsAndValues(currentProgram).containsKey(name)) {
				throw new IllegalStateException("analysis option not found: " + name);
			}
			setAnalysisOption(currentProgram, name, "false");
			println("RECOVER disabled option: " + name);
		}
	}
}
