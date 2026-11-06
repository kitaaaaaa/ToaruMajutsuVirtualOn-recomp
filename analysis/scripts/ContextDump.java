// Prints the instructions before (and for jumps, the references of) each address in a list file.
// Args: <list file: first token of each line is an address>
import java.io.BufferedReader;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;

public class ContextDump extends GhidraScript {
	@Override
	protected void run() throws Exception {
		try (BufferedReader r = new BufferedReader(new FileReader(getScriptArgs()[0]))) {
			String line;
			while ((line = r.readLine()) != null) {
				if (line.trim().isEmpty()) {
					continue;
				}
				Address a = toAddr(Long.decode(line.split(" ")[0]));
				println("CTX ==== " + line);
				Instruction in = getInstructionAt(a);
				if (in == null) {
					in = getInstructionBefore(a);
				}
				List<String> out = new ArrayList<>();
				Instruction p = in;
				for (int i = 0; i < 14 && p != null; i++) {
					out.add(0, "CTX " + p.getAddress() + " " + p);
					p = p.getPrevious();
				}
				for (String s : out) {
					println(s);
				}
				StringBuilder refs = new StringBuilder();
				for (Reference ref : getReferencesTo(a)) {
					refs.append(" ").append(ref.getFromAddress()).append(":").append(ref.getReferenceType());
				}
				if (refs.length() > 0) {
					println("CTX refs_to" + refs);
				}
			}
		}
	}
}
