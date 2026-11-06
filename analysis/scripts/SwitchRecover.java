// Post-script: defines functions from .eh_frame, recovers jump tables from their CMP/JA bounds,
// then exports functions, jump tables, unresolved computed jumps and instruction lengths.
// Args: <export directory containing eh_frame_functions.csv>
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Bookmark;
import ghidra.program.model.listing.BookmarkType;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.FlowOverride;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;

public class SwitchRecover extends GhidraScript {
	private static final int MAX_WALK = 64;
	private static final int MAX_ENTRIES = 4096;

	private static class Table {
		Address jump;
		Address table;
		int entries;
		String boundSource;
		String baseSource;
		long gap = -1;
		List<Address> targets = new ArrayList<>();
		int outsideFde;
	}

	private static class Bound {
		int entries;
		String source;
	}

	private MemoryBlock text;
	private MemoryBlock rodata;
	private int rejected;
	private int ambiguous;
	private long base;
	private final TreeMap<Long, Long> fde = new TreeMap<>();

	@Override
	protected void run() throws Exception {
		File dir = new File(getScriptArgs()[0]);
		text = currentProgram.getMemory().getBlock(".text");
		rodata = currentProgram.getMemory().getBlock(".rodata");
		base =currentProgram.getImageBase().getOffset();
		loadFde(new File(dir, "eh_frame_functions.csv"));

		int created = 0, createFailed = 0;
		for (long start : fde.keySet()) {
			Address a = toAddr(base + start);
			if (getFunctionAt(a) != null) {
				continue;
			}
			if (getInstructionAt(a) == null) {
				disassemble(a);
			}
			if (createFunction(a, null) != null) {
				created++;
			}
			else {
				createFailed++;
			}
		}
		println("RECOVER fde_functions_created=" + created + " create_failed=" + createFailed);

		Map<Address, Table> tables = new HashMap<>();
		for (int pass = 1; pass <= 12; pass++) {
			int found = fixFalseNoReturn() + fixDataConflicts();
			for (Instruction jump : computedJumps()) {
				if (tables.containsKey(jump.getAddress())) {
					continue;
				}
				Table t = recover(jump);
				if (t == null) {
					continue;
				}
				tables.put(jump.getAddress(), t);
				for (Address target : t.targets) {
					currentProgram.getReferenceManager().addMemoryReference(jump.getAddress(), target,
						RefType.COMPUTED_JUMP, SourceType.ANALYSIS, CodeUnit.MNEMONIC);
					if (getInstructionAt(target) == null) {
						disassemble(target);
					}
				}
				found++;
			}
			println("RECOVER pass=" + pass + " new_tables=" + found + " rejected_so_far=" + rejected);
			if (found == 0) {
				break;
			}
			analyzeChanges(currentProgram);
		}

		Set<Function> toFix = new LinkedHashSet<>();
		for (Address j : tables.keySet()) {
			Map.Entry<Long, Long> e = fde.floorEntry(j.getOffset() - base);
			if (e != null && j.getOffset() - base < e.getValue()) {
				Function f = getFunctionAt(toAddr(base + e.getKey()));
				if (f != null) {
					toFix.add(f);
				}
			}
		}
		for (Function f : toFix) {
			CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
		}
		analyzeChanges(currentProgram);

		checkGaps(tables);
		reportNoReturn();
		report(tables);
		export(dir, tables);
	}

	/** Removes data (strings found inside immediates) that blocks a flow of code, and decodes the code. */
	private int fixDataConflicts() throws Exception {
		Pattern conflict = Pattern.compile("conflicting data at ([0-9a-fA-F]+)");
		List<Bookmark> errors = new ArrayList<>();
		Iterator<Bookmark> it = currentProgram.getBookmarkManager().getBookmarksIterator(BookmarkType.ERROR);
		while (it.hasNext()) {
			errors.add(it.next());
		}
		int fixed = 0;
		for (Bookmark b : errors) {
			Matcher m = conflict.matcher(b.getComment());
			if (!m.find() || !text.contains(b.getAddress())) {
				continue;
			}
			Data d = getDataContaining(toAddr(Long.parseLong(m.group(1), 16)));
			if (d == null || !insideFde(d.getAddress())) {
				continue;
			}
			println("RECOVER data_in_code removed " + d.getAddress() + " \"" + d.getDefaultValueRepresentation() + "\" for " + b.getAddress());
			removeData(d);
			Address at = b.getAddress();
			currentProgram.getBookmarkManager().removeBookmark(b);
			disassemble(at);
			fixed++;
		}
		return fixed;
	}

	/** Clears "does not return" from functions that contain a RET and whose callers continue with real code. */
	private int fixFalseNoReturn() throws Exception {
		List<Function> candidates = new ArrayList<>();
		FunctionIterator fi = currentProgram.getFunctionManager().getFunctions(true);
		while (fi.hasNext()) {
			Function f = fi.next();
			if (f.hasNoReturn()) {
				candidates.add(f);
			}
		}
		int fixed = 0;
		for (Function f : candidates) {
			List<Instruction> calls = new ArrayList<>();
			int realAfter = 0;
			for (Reference r : getReferencesTo(f.getEntryPoint())) {
				Instruction c = getInstructionAt(r.getFromAddress());
				if (c == null || !r.getReferenceType().isCall()) {
					continue;
				}
				calls.add(c);
				Address next = c.getMaxAddress().add(1);
				if (getInstructionAt(next) == null && insideFde(next) && !isPadding(next)) {
					realAfter++;
				}
			}
			boolean hasRet = false;
			InstructionIterator body = currentProgram.getListing().getInstructions(f.getBody(), true);
			while (body.hasNext()) {
				Instruction in = body.next();
				hasRet |= in.getMnemonicString().equals("RET");
				// A tail call returns through its target.
				if (in.getMnemonicString().equals("JMP")) {
					for (Reference r : in.getReferencesFrom()) {
						Function target = r.getReferenceType().isFlow() ? getFunctionAt(r.getToAddress()) : null;
						hasRet |= target != null && target != f && !target.hasNoReturn();
					}
				}
			}
			if (realAfter == 0 || !hasRet) {
				if (realAfter > 0) {
					println("RECOVER noreturn_unconfirmed " + f.getEntryPoint() + " " + f.getName() + " calls=" +
						calls.size() + " code_after=" + realAfter);
				}
				continue;
			}
			println("RECOVER noreturn_cleared " + f.getEntryPoint() + " " + f.getName() + " calls=" + calls.size() +
				" code_after=" + realAfter);
			f.setNoReturn(false);
			for (Instruction c : calls) {
				c.setFlowOverride(FlowOverride.NONE);
				Address next = c.getMaxAddress().add(1);
				if (getInstructionAt(next) == null && insideFde(next) && !isPadding(next)) {
					disassemble(next);
				}
			}
			fixed++;
		}
		return fixed;
	}

	private boolean insideFde(Address a) {
		Map.Entry<Long, Long> e = fde.floorEntry(a.getOffset() - base);
		return e != null && a.getOffset() - base < e.getValue();
	}

	private boolean isPadding(Address a) throws Exception {
		int b0 = getByte(a) & 0xff, b1 = getByte(a.add(1)) & 0xff, b2 = getByte(a.add(2)) & 0xff;
		return b0 == 0xcc || b0 == 0x90 || b0 == 0x00 || (b0 == 0x0f && b1 == 0x1f) ||
			(b0 == 0x66 && (b1 == 0x90 || b1 == 0x66 || b1 == 0x2e || (b1 == 0x0f && b2 == 0x1f)));
	}

	private void reportNoReturn() {
		FunctionIterator fi = currentProgram.getFunctionManager().getFunctions(true);
		int count = 0;
		while (fi.hasNext()) {
			Function f = fi.next();
			if (!f.hasNoReturn()) {
				continue;
			}
			count++;
			int calls = 0;
			for (Reference r : getReferencesTo(f.getEntryPoint())) {
				calls += r.getReferenceType().isCall() ? 1 : 0;
			}
			println("RECOVER noreturn " + f.getEntryPoint() + " " + f.getName() + " thunk=" + f.isThunk() + " calls=" + calls);
		}
		println("RECOVER noreturn_functions=" + count);
	}

	private void loadFde(File csv) throws Exception {
		try (BufferedReader r = new BufferedReader(new FileReader(csv))) {
			r.readLine();
			String line;
			while ((line = r.readLine()) != null) {
				String[] p = line.split(",");
				fde.put(Long.decode(p[0]), Long.decode(p[1]));
			}
		}
	}

	private List<Instruction> computedJumps() {
		List<Instruction> out = new ArrayList<>();
		InstructionIterator it = currentProgram.getListing().getInstructions(text.getStart(), true);
		while (it.hasNext()) {
			Instruction in = it.next();
			if (!text.contains(in.getAddress())) {
				break;
			}
			if (in.getFlowType().isComputed() && in.getFlowType().isJump()) {
				out.add(in);
			}
		}
		return out;
	}

	/** Steps backward along control flow. Sets taken[0] when arrival was through a taken jump. */
	private Instruction back(Instruction cur, boolean[] taken) {
		taken[0] = false;
		Instruction p = cur.getPrevious();
		if (p != null && cur.getAddress().equals(p.getFallThrough())) {
			return p;
		}
		Instruction source = null;
		for (Reference r : getReferencesTo(cur.getAddress())) {
			if (r.getReferenceType().isJump() && !r.getReferenceType().isComputed()) {
				if (source != null) {
					return null;
				}
				source = getInstructionAt(r.getFromAddress());
			}
		}
		taken[0] = source != null;
		return source;
	}

	private Register reg(Instruction in, int op) {
		Object[] o = in.getOpObjects(op);
		return o.length == 1 && o[0] instanceof Register ? (Register) o[0] : null;
	}

	private boolean same(Register a, Register b) {
		return a != null && b != null && a.getBaseRegister().equals(b.getBaseRegister());
	}

	private Table recover(Instruction jump) throws Exception {
		if (jump.getNumOperands() != 1) {
			return null;
		}
		Register rt = reg(jump, 0);
		if (rt == null) {
			return null;
		}
		boolean[] taken = new boolean[1];
		Register rb = null, ri = null;
		Instruction cur = jump, movsxd = null;
		for (int i = 0; i < MAX_WALK && movsxd == null; i++) {
			cur = back(cur, taken);
			if (cur == null) {
				return null;
			}
			String m = cur.getMnemonicString();
			if (rb == null && m.equals("ADD") && same(reg(cur, 0), rt) && reg(cur, 1) != null) {
				rb = reg(cur, 1);
			}
			else if (rb != null && m.equals("MOVSXD") && same(reg(cur, 0), rt)) {
				Object[] o = cur.getOpObjects(1);
				boolean hasBase = false, hasScale4 = false;
				for (Object x : o) {
					if (x instanceof Register && same((Register) x, rb)) {
						hasBase = true;
					}
					else if (x instanceof Register) {
						ri = (Register) x;
					}
					else if (x instanceof Scalar && ((Scalar) x).getValue() == 4) {
						hasScale4 = true;
					}
				}
				if (!hasBase || !hasScale4 || ri == null) {
					return null;
				}
				movsxd = cur;
			}
		}
		if (movsxd == null) {
			return null;
		}

		Map.Entry<Long, Long> range = fde.floorEntry(jump.getAddress().getOffset() - base);
		if (range == null || jump.getAddress().getOffset() - base >= range.getValue()) {
			return null;
		}
		Bound b = findBound(movsxd, ri);

		// The base register, followed back through copies along control flow.
		Address table = null;
		Register tracked = rb;
		cur = movsxd;
		for (int i = 0; i < MAX_WALK && table == null; i++) {
			cur = back(cur, taken);
			if (cur == null) {
				break;
			}
			String m = cur.getMnemonicString();
			if (m.equals("LEA") && same(reg(cur, 0), tracked)) {
				Reference pr = cur.getPrimaryReference(1);
				if (pr != null && rodata.contains(pr.getToAddress())) {
					table = pr.getToAddress();
				}
				break;
			}
			if (m.equals("MOV") && same(reg(cur, 0), tracked)) {
				tracked = reg(cur, 1);
				if (tracked == null) {
					break;
				}
			}
		}
		String baseSource = "linear";
		if (b != null && (table == null || !allInside(table, b.entries, range))) {
			// A base loaded outside the loop or block: the function's other LEAs of the register.
			table = null;
			baseSource = "function_scan";
			Address nearest = null;
			int valid = 0;
			InstructionIterator it = currentProgram.getListing().getInstructions(toAddr(base + range.getKey()), true);
			while (it.hasNext()) {
				Instruction in = it.next();
				if (in.getAddress().getOffset() - base >= range.getValue()) {
					break;
				}
				if (!in.getMnemonicString().equals("LEA") || !(same(reg(in, 0), rb) || same(reg(in, 0), tracked))) {
					continue;
				}
				Reference pr = in.getPrimaryReference(1);
				if (pr == null || !rodata.contains(pr.getToAddress()) || !allInside(pr.getToAddress(), b.entries, range)) {
					continue;
				}
				if (table == null || !pr.getToAddress().equals(table)) {
					valid++;
				}
				if (in.getAddress().compareTo(jump.getAddress()) < 0) {
					nearest = pr.getToAddress();
				}
				table = pr.getToAddress();
			}
			if (nearest != null) {
				table = nearest;
			}
			if (table == null) {
				rejected++;
				return null;
			}
			if (valid > 1) {
				ambiguous++;
				println("RECOVER ambiguous_base jump=" + jump.getAddress() + " chosen=" + table + " candidates=" + valid);
			}
		}
		if (table == null) {
			return null;
		}

		Table t = new Table();
		t.jump = jump.getAddress();
		t.table = table;
		t.baseSource = baseSource;
		int limit = b != null ? b.entries : MAX_ENTRIES;
		t.boundSource = b != null ? b.source : "fde_scan";
		for (int i = 0; i < limit; i++) {
			Address entry = table.add(i * 4L);
			if (!currentProgram.getMemory().contains(entry)) {
				break;
			}
			Address target = table.add((long) getInt(entry));
			if (target.getOffset() - base < range.getKey() || target.getOffset() - base >= range.getValue()) {
				break;
			}
			t.targets.add(target);
		}
		t.entries = t.targets.size();
		return t.entries == 0 ? null : t;
	}

	/** Whether every entry of a table lands inside the function's .eh_frame range. */
	private boolean allInside(Address table, int entries, Map.Entry<Long, Long> range) throws Exception {
		for (int i = 0; i < entries; i++) {
			Address entry = table.add(i * 4L);
			if (!rodata.contains(entry)) {
				return false;
			}
			long target = table.getOffset() + getInt(entry) - base;
			if (target < range.getKey() || target >= range.getValue()) {
				return false;
			}
		}
		return true;
	}

	/** Whether an instruction leaves the flags alone, so a CMP before it still decides a later branch. */
	private boolean keepsFlags(String m) {
		if (m.startsWith("VUCOMIS") || m.startsWith("VCOMIS")) {
			return false;
		}
		return m.startsWith("MOV") || m.startsWith("V") || m.equals("LEA") || m.equals("NOP") || m.equals("PUSH") ||
			m.equals("POP");
	}

	private Bound findBound(Instruction from, Register ri) {
		boolean[] taken = new boolean[1];
		boolean sawBextr = false;
		Instruction cur = from;
		for (int i = 0; i < MAX_WALK; i++) {
			Instruction next = back(cur, taken);
			if (next == null) {
				return null;
			}
			boolean viaTaken = taken[0];
			cur = next;
			String m = cur.getMnemonicString();
			if (m.equals("BEXTR") && same(reg(cur, 0), ri)) {
				sawBextr = true;
			}
			int adjust;
			if (!viaTaken && m.equals("JA") || viaTaken && m.equals("JBE")) {
				adjust = 1;
			}
			else if (!viaTaken && m.equals("JNC") || viaTaken && m.equals("JC") ||
				!viaTaken && m.equals("JAE") || viaTaken && m.equals("JB")) {
				adjust = 0;
			}
			else {
				continue;
			}
			Instruction c = cur;
			for (int k = 0; k < 16; k++) {
				c = c.getPrevious();
				if (c == null) {
					return null;
				}
				if (c.getMnemonicString().equals("CMP")) {
					Object[] imm = c.getOpObjects(1);
					Register cr = reg(c, 0);
					if (cr == null || imm.length != 1 || !(imm[0] instanceof Scalar)) {
						return null;
					}
					long mask = cr.getBitLength() >= 64 ? -1L : (1L << cr.getBitLength()) - 1;
					long v = ((Scalar) imm[0]).getValue() & mask;
					if (v < 0 || v + adjust > MAX_ENTRIES) {
						return null;
					}
					Instruction d = c;
					for (int q = 0; q < 8 && d != null && !sawBextr; q++) {
						d = d.getPrevious();
						sawBextr = d != null && d.getMnemonicString().equals("BEXTR") && same(reg(d, 0), ri);
					}
					Bound b = new Bound();
					b.entries = (int) v + adjust;
					b.source = same(cr, ri) ? "cmp_direct" : sawBextr ? "cmp_bextr" : "cmp_nearest";
					return b;
				}
				if (!keepsFlags(c.getMnemonicString())) {
					return null;
				}
			}
			return null;
		}
		return null;
	}

	private void checkGaps(Map<Address, Table> tables) {
		TreeMap<Long, Long> starts = new TreeMap<>();
		for (Table t : tables.values()) {
			starts.put(t.table.getOffset(), 0L);
		}
		for (Table t : tables.values()) {
			Long next = starts.higherKey(t.table.getOffset());
			if (next != null) {
				t.gap = next - t.table.getOffset();
			}
		}
	}

	private void report(Map<Address, Table> tables) {
		Map<String, Integer> bySource = new TreeMap<>();
		Map<String, Integer> byBase = new TreeMap<>();
		for (Table t : tables.values()) {
			byBase.merge(t.baseSource, 1, Integer::sum);
		}
		println("RECOVER by_base=" + byBase + " rejected=" + rejected + " ambiguous_base=" + ambiguous);
		int exact = 0, under = 0, over = 0, outside = 0, midInstruction = 0, undecoded = 0;
		Set<Address> shared = new HashSet<>();
		int duplicateTables = 0;
		for (Table t : tables.values()) {
			bySource.merge(t.boundSource, 1, Integer::sum);
			if (!shared.add(t.table)) {
				duplicateTables++;
			}
			if (t.gap >= 0) {
				if (t.entries * 4L == t.gap) {
					exact++;
				}
				else if (t.entries * 4L < t.gap) {
					under++;
				}
				else {
					over++;
					println("RECOVER gap_overrun jump=" + t.jump + " table=" + t.table + " entries=" + t.entries +
						" gap=" + t.gap + " src=" + t.boundSource);
				}
			}
			if (t.outsideFde > 0) {
				outside++;
				println("RECOVER outside_fde jump=" + t.jump + " table=" + t.table + " entries=" + t.entries +
					" outside=" + t.outsideFde + " src=" + t.boundSource);
			}
			for (Address a : t.targets) {
				if (getInstructionAt(a) == null) {
					if (getInstructionContaining(a) != null) {
						midInstruction++;
					}
					else {
						undecoded++;
					}
				}
			}
		}
		println("RECOVER tables=" + tables.size() + " by_source=" + bySource + " shared_table_jumps=" + duplicateTables);
		println("RECOVER gap_exact=" + exact + " gap_under=" + under + " gap_over=" + over +
			" tables_with_targets_outside_fde=" + outside);
		println("RECOVER targets_mid_instruction=" + midInstruction + " targets_undecoded=" + undecoded);

		Map<String, Integer> cats = new TreeMap<>();
		Iterator<Bookmark> it = currentProgram.getBookmarkManager().getBookmarksIterator(BookmarkType.ERROR);
		int shown = 0;
		while (it.hasNext()) {
			Bookmark b = it.next();
			cats.merge(b.getCategory(), 1, Integer::sum);
			if (shown++ < 30) {
				String c = b.getComment();
				println("RECOVER error " + b.getAddress() + " " + (c.length() > 110 ? c.substring(0, 110) : c));
			}
		}
		println("RECOVER error_bookmarks=" + cats);

		long instBytes = 0;
		InstructionIterator ii = currentProgram.getListing().getInstructions(text.getStart(), true);
		while (ii.hasNext()) {
			Instruction in = ii.next();
			if (!text.contains(in.getAddress())) {
				break;
			}
			instBytes += in.getLength();
		}
		println("RECOVER text_size=" + text.getSize() + " instruction_bytes=" + instBytes);
	}

	private void export(File dir, Map<Address, Table> tables) throws Exception {
		try (PrintWriter w = new PrintWriter(new File(dir, "jump_tables.csv"))) {
			w.println("jump,table,entries,bound_source,base_source,gap_bytes,outside_fde,targets");
			for (Table t : new java.util.TreeMap<>(tables).values()) {
				StringBuilder s = new StringBuilder();
				for (Address a : t.targets) {
					s.append(s.length() == 0 ? "" : ";").append("0x").append(a);
				}
				w.println("0x" + t.jump + ",0x" + t.table + "," + t.entries + "," + t.boundSource + "," + t.baseSource + "," + t.gap + "," +
					t.outsideFde + "," + s);
			}
		}

		int resolved = 0, unresolved = 0;
		Map<String, Integer> forms = new TreeMap<>();
		try (PrintWriter w = new PrintWriter(new File(dir, "computed_jumps_unresolved.csv"))) {
			w.println("address,function,instruction");
			for (Instruction j : computedJumps()) {
				if (tables.containsKey(j.getAddress())) {
					resolved++;
					continue;
				}
				unresolved++;
				Function f = getFunctionContaining(j.getAddress());
				boolean registerForm = reg(j, 0) != null;
				boolean thunk = f != null && f.isThunk();
				forms.merge((registerForm ? "reg" : "mem") + (thunk ? "_thunk" : ""), 1, Integer::sum);
				w.println("0x" + j.getAddress() + "," + (f == null ? "" : "0x" + f.getEntryPoint()) + ",\"" + j + "\"");
			}
		}
		println("RECOVER computed_jumps resolved=" + resolved + " unresolved=" + unresolved + " forms=" + forms);

		int total = 0, thunks = 0, inFde = 0, entryNotFde = 0, bodyOutside = 0, fdeMissing = 0;
		Set<Long> entries = new HashSet<>();
		try (PrintWriter w = new PrintWriter(new File(dir, "functions.csv"))) {
			w.println("entry,body_end,thunk,name,fde_start,fde_end,status");
			FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
			while (it.hasNext()) {
				Function f = it.next();
				total++;
				long start = f.getEntryPoint().getOffset() - base;
				entries.add(start);
				long end = f.getBody().getMaxAddress().getOffset() - base + 1;
				long min = f.getBody().getMinAddress().getOffset() - base;
				if (f.isThunk()) {
					thunks++;
				}
				Long fdeEnd = fde.get(start);
				String status;
				if (fdeEnd == null) {
					entryNotFde++;
					status = insideFde(f.getEntryPoint()) ? "no_fde_inside_other_range" : "no_fde";
				}
				else if (min < start || end > fdeEnd) {
					bodyOutside++;
					status = "body_outside_fde";
				}
				else {
					inFde++;
					status = "ok";
				}
				w.println("0x" + f.getEntryPoint() + ",0x" + Long.toHexString(base + end) + "," + f.isThunk() + ",\"" +
					f.getName() + "\"," + (fdeEnd == null ? "," : "0x" + Long.toHexString(base + start) + ",0x" +
						Long.toHexString(base + fdeEnd)) + "," + status);
			}
		}
		for (long s : fde.keySet()) {
			if (!entries.contains(s)) {
				fdeMissing++;
			}
		}
		println("RECOVER functions=" + total + " thunks=" + thunks + " ok=" + inFde + " no_fde=" + entryNotFde +
			" body_outside_fde=" + bodyOutside + " fde_without_function=" + fdeMissing);

		try (PrintWriter w = new PrintWriter(new File(dir, "instructions.csv"))) {
			w.println("address,length,mnemonic");
			InstructionIterator ii = currentProgram.getListing().getInstructions(text.getStart(), true);
			while (ii.hasNext()) {
				Instruction in = ii.next();
				if (!text.contains(in.getAddress())) {
					break;
				}
				w.println("0x" + in.getAddress() + "," + in.getLength() + "," + in.getMnemonicString());
			}
		}
		println("RECOVER export complete");
	}
}
