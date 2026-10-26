// Recovers Borland switch jump tables that Ghidra misreads, after auto-analysis.
//
// Borland compiles a dense switch as
//     cmp bx, N ; ja default (or jbe ok / jmp default) ; shl bx, 1 ; jmp word ptr cs:[bx + table]
// with the table of near offsets in the code segment. Ghidra's 16-bit x86 model derives CS from
// the instruction address rounded down to 64 KB, not from the real segment, so it reads the table
// from the wrong place and the decompiler chases garbage targets ("Bad instruction count exceeded").
// This script reads each table from the segment the manifest says the code lives in, disassembles
// the cases and records a jump-table override that the decompiler uses instead of its own lookup.
// Args: <manifest.json>
//@category WC

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.SegmentedAddress;
import ghidra.program.model.address.SegmentedAddressSpace;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.FlowOverride;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.pcode.JumpTable;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;

public class WcSwitches extends GhidraScript {

	private record Segment(int seg, int start, int end) {}

	private SegmentedAddressSpace space;
	private final List<Segment> segments = new ArrayList<>();

	@Override
	protected void run() throws Exception {
		JsonObject manifest = JsonParser.parseString(Files.readString(Path.of(getScriptArgs()[0])))
				.getAsJsonObject();
		space = (SegmentedAddressSpace) currentProgram.getAddressFactory().getDefaultAddressSpace();
		int load = loadSegment(manifest);
		for (JsonElement e : manifest.getAsJsonArray("segments")) {
			JsonObject s = e.getAsJsonObject();
			String kind = s.get("kind").getAsString();
			if (kind.equals("code") || kind.equals("overlay")) {
				int seg = load + s.get("seg").getAsInt();
				segments.add(new Segment(seg, seg * 16 + s.get("minOff").getAsInt(),
					seg * 16 + s.get("size").getAsInt()));
			}
		}

		// Case bodies can contain further switches, and re-analysis of the new code can split
		// functions, so repeat until nothing new turns up and only then attach the overrides.
		Set<Address> done = new HashSet<>();
		Map<Address, List<Address>> tables = new LinkedHashMap<>();
		int skipped = 0;
		while (true) {
			List<Instruction> pending = new ArrayList<>();
			for (Instruction ins : currentProgram.getListing().getInstructions(true)) {
				if (!done.contains(ins.getAddress()) && isSwitchJump(ins)) {
					pending.add(ins);
				}
			}
			if (pending.isEmpty()) {
				break;
			}
			for (Instruction ins : pending) {
				monitor.checkCancelled();
				done.add(ins.getAddress());
				List<Address> targets = recover(ins);
				if (targets != null) {
					tables.put(ins.getAddress(), targets);
				}
				else {
					skipped++;
				}
			}
			analyzeChanges(currentProgram);
		}

		Set<Function> switchFunctions = new LinkedHashSet<>();
		for (Address at : tables.keySet()) {
			Function f = getFunctionContaining(at);
			if (f != null) {
				switchFunctions.add(f);
			}
		}
		// "Shared Return Calls" is what turned those JMPs into tail calls. Headless runs analysis
		// again on pending changes after the post-scripts, and it would split the merged functions
		// again in the saved project, so turn it off before merging.
		setAnalysisOption(currentProgram, "Shared Return Calls", "false");
		int merged = 0;
		for (Function f : switchFunctions) {
			if (!f.isDeleted()) {
				merged += mergeFragments(f);
			}
		}
		analyzeChanges(currentProgram);

		int orphaned = 0;
		for (var e : tables.entrySet()) {
			Function f = getFunctionContaining(e.getKey());
			if (f == null) {
				orphaned++;
				continue;
			}
			CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
			new JumpTable(e.getKey(), new ArrayList<>(e.getValue()), true, 0).writeOverride(f);
		}
		writeTables(Path.of(getScriptArgs()[0]).resolveSibling("switches.json"), tables);
		println(String.format(
			"WcSwitches: recovered %d jump tables, skipped %d, %d outside any function, merged %d fragments",
			tables.size(), skipped, orphaned, merged));
	}

	/** For WcDecompile: each table's jump address and case targets in index order, as linear addresses. */
	private static void writeTables(Path path, Map<Address, List<Address>> tables) throws Exception {
		JsonArray out = new JsonArray();
		for (var e : tables.entrySet()) {
			JsonObject t = new JsonObject();
			t.addProperty("jump", e.getKey().getOffset());
			JsonArray targets = new JsonArray();
			e.getValue().forEach(a -> targets.add(a.getOffset()));
			t.add("targets", targets);
			out.add(t);
		}
		Files.writeString(path, out.toString());
	}

	/**
	 * Before the tables were known, analysis saw only part of each switch function. It then made
	 * function starts out of code the function jumps to (loop heads, case bodies) and turned those
	 * JMPs into tail calls ("shared return calls"). Walk f's real control flow and absorb every
	 * such fragment: a default-named function that is never CALLed or referenced from data and is
	 * only jumped to from code inside f. Returns the number of fragments absorbed.
	 */
	private int mergeFragments(Function f) throws Exception {
		Set<Function> excluded = new HashSet<>();
		while (true) {
			Set<Address> reach = new HashSet<>();
			Set<Function> absorbed = new LinkedHashSet<>();
			Deque<Address> work = new ArrayDeque<>(List.of(f.getEntryPoint()));
			while (!work.isEmpty() && reach.size() < 50000) {
				Address a = work.pop();
				Instruction ins = getInstructionAt(a);
				if (ins == null || reach.contains(a)) {
					continue;
				}
				Function g = getFunctionAt(a);
				if (g != null && !g.equals(f)) {
					if (excluded.contains(g) || !isFragment(g)) {
						continue;
					}
					absorbed.add(g);
				}
				reach.add(a);
				if (ins.getMnemonicString().startsWith("CALL")) {
					if (ins.getDefaultFallThrough() != null) {
						work.push(ins.getDefaultFallThrough());
					}
					continue;
				}
				for (Address t : ins.getFlows()) {
					work.push(t);
				}
				for (Reference r : getReferencesFrom(a)) {
					if (r.getReferenceType().isJump()) {
						work.push(r.getToAddress());
					}
				}
				if (ins.getDefaultFallThrough() != null && !ins.getFlowType().isTerminal()) {
					work.push(ins.getDefaultFallThrough());
				}
			}

			Function outside = null;
			for (Function g : absorbed) {
				for (Reference r : getReferencesTo(g.getEntryPoint())) {
					if (!reach.contains(r.getFromAddress())) {
						outside = g;
					}
				}
			}
			if (outside != null) {
				excluded.add(outside);
				continue;
			}
			if (absorbed.isEmpty()) {
				return 0;
			}

			for (Function g : absorbed) {
				removeFunction(g);
			}
			for (Address a : reach) {
				Instruction ins = getInstructionAt(a);
				if (ins.getFlowOverride() != FlowOverride.NONE && !ins.getMnemonicString().startsWith("CALL")) {
					ins.setFlowOverride(FlowOverride.NONE);
				}
			}
			CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
			return absorbed.size();
		}
	}

	/** A default-named function reached only by near jumps: no calls, far jumps, data refs or entry point. */
	private boolean isFragment(Function g) {
		if (g.getSymbol().getSource() != SourceType.DEFAULT || g.isThunk() ||
			currentProgram.getSymbolTable().isExternalEntryPoint(g.getEntryPoint())) {
			return false;
		}
		for (Reference r : getReferencesTo(g.getEntryPoint())) {
			Instruction from = getInstructionAt(r.getFromAddress());
			if (from == null) {
				return false;
			}
			String m = from.getMnemonicString();
			if (m.startsWith("CALL") || m.equals("JMPF")) {
				return false;
			}
		}
		return true;
	}

	/** jmp word ptr cs:[bx + disp16]: 2E FF A7 lo hi. */
	private boolean isSwitchJump(Instruction ins) {
		try {
			byte[] b = ins.getBytes();
			return b.length == 5 && b[0] == 0x2e && b[1] == (byte) 0xff && b[2] == (byte) 0xa7;
		}
		catch (Exception e) {
			return false;
		}
	}

	/** Reads and disassembles one jump table; returns its targets, or null if it doesn't fit the pattern. */
	private List<Address> recover(Instruction jmp) throws Exception {
		Address at = jmp.getAddress();
		Segment seg = segmentOf(at.getOffset());
		Integer cases = caseCount(jmp);
		if (seg == null || cases == null) {
			println("WcSwitches: skipped " + at + (seg == null ? " (no segment)" : " (no cmp bx,N bound)"));
			return null;
		}

		int table = jmp.getUnsignedShort(3);
		List<Address> targets = new ArrayList<>();
		for (int i = 0; i < cases; i++) {
			Address entry = space.getAddress(seg.seg, table + i * 2);
			int off = getShort(entry) & 0xffff;
			long linear = seg.seg * 16L + off;
			if (linear < seg.start || linear >= seg.end) {
				println(String.format("WcSwitches: skipped %s (case %d -> %04x outside segment)", at, i, off));
				return null;
			}
			targets.add(space.getAddress(seg.seg, off));
		}

		for (int i = 0; i < cases; i++) {
			Address entry = space.getAddress(seg.seg, table + i * 2);
			if (getInstructionContaining(entry) == null && getDataContaining(entry) == null) {
				createWord(entry);
			}
		}
		// Drop targets Ghidra's own switch analysis guessed from the misread table; some come out
		// as COMPUTED_CALL_TERMINATOR, which makes the jump look like a tail call.
		for (Reference r : getReferencesFrom(at)) {
			if (r.getReferenceType().isComputed() && !targets.contains(r.getToAddress())) {
				removeReference(r);
			}
		}
		for (Address t : new HashSet<>(targets)) {
			currentProgram.getReferenceManager()
					.addMemoryReference(at, t, RefType.COMPUTED_JUMP, SourceType.ANALYSIS, CodeUnit.MNEMONIC);
			disassemble(t);
		}
		return targets;
	}

	/** N+1 from the "cmp bx, N" a few instructions before "shl bx, 1 ; jmp". */
	private Integer caseCount(Instruction jmp) {
		Register bx = currentProgram.getRegister("BX");
		Instruction ins = jmp;
		for (int i = 0; i < 6; i++) {
			ins = getInstructionBefore(ins);
			if (ins == null) {
				return null;
			}
			if (ins.getMnemonicString().equals("CMP") && bx.equals(ins.getRegister(0))) {
				Scalar n = ins.getScalar(1);
				return n == null ? null : (int) n.getUnsignedValue() + 1;
			}
		}
		return null;
	}

	private Segment segmentOf(long linear) {
		Segment best = null;
		for (Segment s : segments) {
			if (s.start <= linear && linear < s.end && (best == null || s.start > best.start)) {
				best = s;
			}
		}
		return best;
	}

	private int loadSegment(JsonObject manifest) {
		JsonObject entry = manifest.getAsJsonObject("entry");
		var it = currentProgram.getSymbolTable().getExternalEntryPointIterator();
		while (it.hasNext()) {
			Address a = it.next();
			if (a instanceof SegmentedAddress sa && sa.getSegmentOffset() == entry.get("ip").getAsInt()) {
				return sa.getSegment() - entry.get("cs").getAsInt();
			}
		}
		throw new IllegalStateException("cannot find the program entry point");
	}
}
