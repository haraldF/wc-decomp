// Names Borland Turbo C++ (medium model) runtime functions and globals in WC_FLAT.EXE by rules.
// Runs after WcFid.java and only fills in what Function ID didn't name (tiny functions, main,
// globals), or everything if no Function ID database was built:
//  - startup: main, exit and _argc/_argv/_environ from the entry code;
//  - exit's atexit table: atexit, _atexitcnt, _atexittbl;
//  - abort: the function that prints "Abnormal program termination";
//  - __IOerror (DOS error -> errno/_doserrno), errno and _doserrno;
//  - DOS wrappers: small far functions with a single INT 21h, named by AH, if the AH is unique;
//  - compiler helpers (struct copy, long arithmetic, huge pointers) by a hash of their exact bytes in WC.EXE.
// Only functions that still have default names are renamed, so names set in the GUI are kept.
// Every name gets a plate comment saying which rule produced it.
// Args: <manifest.json>
//@category WC

import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HexFormat;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.plugin.core.clear.ClearFlowAndRepairCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.SegmentedAddress;
import ghidra.program.model.address.SegmentedAddressSpace;
import ghidra.program.model.data.CharDataType;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.IntegerDataType;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.listing.FlowOverride;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.listing.ParameterImpl;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;
import ghidra.util.exception.DuplicateNameException;

public class WcRuntime extends GhidraScript {

	/** Leading code bytes, by their length and SHA-256 (a prefix of it), so no code is copied here. */
	private record Sig(int length, String sha256) {}

	/** Compiler helpers, by their leading bytes in WC.EXE. */
	private static final Map<String, Sig> HELPERS = new LinkedHashMap<>();
	static {
		HELPERS.put("SCOPY@", new Sig(28, "38d2310516257b58")); // struct copy (far ptrs)
		HELPERS.put("F_SPUSH@", new Sig(33, "496a8748e8f55172")); // struct by value
		HELPERS.put("LXMUL@", new Sig(23, "795b814e02d65368")); // DX:AX * CX:BX
		HELPERS.put("N_LXMUL@", new Sig(23, "50ba43654f43e777"));
		HELPERS.put("LXLSH@", new Sig(30, "ea7e9a96414a68b8")); // DX:AX << CL
		HELPERS.put("LXRSH@", new Sig(29, "3ce015efa0073513")); // DX:AX >> CL, signed
		HELPERS.put("F_PCMP@", new Sig(33, "99e1d684e37b744d")); // huge ptr compare
		HELPERS.put("N_PCMP@", new Sig(33, "20cc3133369e1ddb"));
		HELPERS.put("PADD@", new Sig(24, "2900b91196a7886b")); // huge ptr + long
		HELPERS.put("PSUB@", new Sig(24, "525665e9c62d7f2c")); // huge ptr - long
		// pascal convention, so the link name is uppercase without a leading underscore
		HELPERS.put("__IOERROR", new Sig(61, "13453c8382bbd117"));
	}

	/** Shared body of LDIV@/LUDIV@/LMOD@/LUMOD@; the entry stubs pass the operation in CX. */
	private static final Sig LDIV_COMMON = new Sig(16, "a6792849136a193d");
	private static final String[] LDIV_STUBS = { "LDIV@", "LUDIV@", "LMOD@", "LUMOD@" };

	/** DOS INT 21h function -> Borland runtime function that is a thin wrapper around it. */
	private static final Map<Integer, String> DOS = Map.ofEntries(
		Map.entry(0x07, "getch"), Map.entry(0x0b, "kbhit"), Map.entry(0x25, "setvect"),
		Map.entry(0x2a, "getdate"), Map.entry(0x2c, "gettime"), Map.entry(0x35, "getvect"),
		Map.entry(0x3b, "chdir"), Map.entry(0x3c, "_creat"), Map.entry(0x3d, "_open"),
		Map.entry(0x3e, "_close"), Map.entry(0x3f, "_read"), Map.entry(0x40, "_write"),
		Map.entry(0x41, "unlink"), Map.entry(0x42, "lseek"), Map.entry(0x43, "_chmod"),
		Map.entry(0x44, "ioctl"), Map.entry(0x4a, "setblock"), Map.entry(0x4c, "_exit"));
	private static final String[] DOS_DESC = new String[256];
	static {
		DOS_DESC[0x07] = "direct console input, no echo";
		DOS_DESC[0x0b] = "check input status";
		DOS_DESC[0x25] = "set interrupt vector";
		DOS_DESC[0x2a] = "get date";
		DOS_DESC[0x2c] = "get time";
		DOS_DESC[0x35] = "get interrupt vector";
		DOS_DESC[0x3b] = "change directory";
		DOS_DESC[0x3c] = "create file";
		DOS_DESC[0x3d] = "open file";
		DOS_DESC[0x3e] = "close file";
		DOS_DESC[0x3f] = "read file";
		DOS_DESC[0x40] = "write file";
		DOS_DESC[0x41] = "delete file";
		DOS_DESC[0x42] = "seek";
		DOS_DESC[0x43] = "get/set file attributes";
		DOS_DESC[0x44] = "IOCTL";
		DOS_DESC[0x4a] = "resize memory block";
		DOS_DESC[0x4c] = "terminate process";
	}
	private static final int DOS_WRAPPER_MAX_BYTES = 80;

	private SegmentedAddressSpace space;
	private int dgroup;
	private long rtlStart, rtlEnd;
	private final List<String> named = new ArrayList<>();

	@Override
	protected void run() throws Exception {
		JsonObject manifest = JsonParser.parseString(Files.readString(Path.of(getScriptArgs()[0])))
				.getAsJsonObject();
		space = (SegmentedAddressSpace) currentProgram.getAddressFactory().getDefaultAddressSpace();
		int load = loadSegment(manifest);
		if (manifest.get("dgroup").isJsonNull()) {
			throw new IllegalStateException("manifest has no DGROUP");
		}
		dgroup = load + manifest.get("dgroup").getAsInt();
		for (var e : manifest.getAsJsonArray("segments")) {
			JsonObject s = e.getAsJsonObject();
			if (s.get("seg").getAsInt() == 0 && s.get("kind").getAsString().equals("code")) {
				rtlStart = load * 16L;
				rtlEnd = rtlStart + s.get("size").getAsInt();
			}
		}

		Function entry = getFunctionAt(entryPoint(manifest, load));
		Function exit = startup(entry);
		Function abort = abort();
		Function exitToDos = dosWrappers().get(0x4c);
		repairNoReturn(List.of(exit, abort, exitToDos));
		if (exit != null) {
			atexit(exit);
		}
		helpers();

		println("WcRuntime: named " + named.size() + " runtime functions and globals");
		for (String n : named) {
			println("WcRuntime:   " + n);
		}
	}

	/** main is far-called after pushing three words (envp, argv, argc); exit is called with its result. */
	private Function startup(Function entry) throws Exception {
		Instruction prev3 = null, prev2 = null, prev1 = null;
		Function main = null, exit = null;
		for (Instruction ins : currentProgram.getListing().getInstructions(entry.getBody(), true)) {
			if (ins.getMnemonicString().equals("CALLF") && ins.getFlows().length == 1) {
				Function target = calledFunction(ins);
				if (main == null && isPushMem(prev1) && isPushMem(prev2) && isPushMem(prev3)) {
					main = target;
					nameFunction(main, "main", "Called by the startup code with _argc, _argv, _environ.");
					labelGlobal(memOperand(prev1), "_argc", "argc for main");
					labelGlobal(memOperand(prev2), "_argv", "argv for main");
					labelGlobal(memOperand(prev3), "_environ", "envp for main");
					setMainSignature(main);
				}
				else if (main != null && exit == null && prev1 != null && prev1.toString().equals("PUSH AX")) {
					exit = target;
					nameFunction(exit, "exit", "Called by the startup code with main's return value.");
				}
			}
			prev3 = prev2;
			prev2 = prev1;
			prev1 = ins;
		}
		if (main == null) {
			println("WcRuntime: could not find the call to main in the startup code");
		}
		return exit;
	}

	/**
	 * exit runs the atexit functions: "DEC word ptr [cnt]" and "CALLF [BX + tbl]". atexit is the
	 * function that checks the same counter against the table size (32).
	 */
	private void atexit(Function exit) throws Exception {
		Long count = null, table = null;
		for (Instruction ins : currentProgram.getListing().getInstructions(exit.getBody(), true)) {
			if (ins.getMnemonicString().equals("DEC") && ins.toString().contains("word ptr [0x")) {
				count = memOperand(ins);
			}
			if (ins.getMnemonicString().equals("CALLF") && ins.toString().contains("[BX + 0x")) {
				table = memOperand(ins);
			}
		}
		if (count == null) {
			return;
		}
		labelGlobal(count, "_atexitcnt", "number of registered atexit functions");
		if (table != null) {
			labelGlobal(table, "_atexittbl", "atexit function table (far pointers)");
		}
		String check = String.format("CMP word ptr [0x%x],0x20", count);
		for (Function f : runtimeFunctions()) {
			if (f.equals(exit)) {
				continue;
			}
			for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
				if (ins.toString().equals(check)) {
					nameFunction(f, "atexit", "Adds to _atexittbl if fewer than 32 are registered.");
					return;
				}
			}
		}
	}

	/** abort prints "Abnormal program termination\r\n" (MOV CX,len; MOV DX,msg) and exits with 3. */
	private Function abort() throws Exception {
		byte[] msg = "Abnormal program termination".getBytes("ISO-8859-1");
		Address dg = space.getAddress(dgroup, 0);
		var block = currentProgram.getMemory().getBlock(dg);
		if (block == null) {
			return null;
		}
		Address end = space.getAddress(dgroup, 0xffff);
		Address found = currentProgram.getMemory().findBytes(dg, end.compareTo(block.getEnd()) < 0 ? end : block.getEnd(),
			msg, null, true, monitor);
		if (found == null) {
			return null;
		}
		String movDx = String.format("MOV DX,0x%x", found.getOffset() - dg.getOffset());
		for (Function f : runtimeFunctions()) {
			for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
				if (ins.toString().equals(movDx)) {
					nameFunction(f, "abort", "Prints \"Abnormal program termination\" and exits with code 3.");
					return f;
				}
			}
		}
		return null;
	}

	/**
	 * Ghidra doesn't know these functions never return, so it decodes whatever follows a call to
	 * them as code. After abort's call to _exit, for example, come a CS-resident data word, the
	 * "Runtime overlay error" message and other functions, and the misaligned decode swallowed
	 * them (and atexit) into abort's body. Mark the functions non-returning, end the flow at each
	 * call, clear the code after a call where nothing else jumps or calls to it, and re-create
	 * runtime functions whose callers now point at undefined bytes.
	 */
	private void repairNoReturn(List<Function> noReturn) throws Exception {
		int cleared = 0;
		for (Function f : noReturn) {
			if (f == null) {
				continue;
			}
			f.setNoReturn(true);
			for (Reference r : getReferencesTo(f.getEntryPoint())) {
				Instruction call = getInstructionAt(r.getFromAddress());
				if (call == null || !r.getReferenceType().isCall()) {
					continue;
				}
				Address after = call.getDefaultFallThrough();
				call.setFlowOverride(FlowOverride.CALL_RETURN);
				if (after != null && getInstructionAt(after) != null && !hasFlowInto(after)) {
					println("WcRuntime: clearing code after " + f.getName() + " call at " + call.getAddress());
					runCommand(new ClearFlowAndRepairCmd(after, false, false, true));
					cleared++;
				}
				Function caller = getFunctionContaining(call.getAddress());
				if (caller != null) {
					CreateFunctionCmd.fixupFunctionBody(currentProgram, caller, monitor);
				}
			}
		}
		Set<Address> missing = new TreeSet<>();
		for (Reference r : currentProgram.getReferenceManager().getReferenceIterator(space.getAddress(0, 0))) {
			Address to = r.getToAddress();
			if (r.getReferenceType().isCall() && to.getOffset() >= rtlStart && to.getOffset() < rtlEnd &&
				getFunctionAt(to) == null && getInstructionAt(r.getFromAddress()) != null) {
				missing.add(to);
			}
		}
		int created = 0;
		for (Address to : missing) {
			disassemble(to);
			if (getInstructionAt(to) != null && getFunctionAt(to) == null && createFunction(to, null) != null) {
				created++;
			}
		}
		println(String.format("WcRuntime: marked %d functions non-returning, cleared code after %d calls, " +
			"re-created %d functions", noReturn.stream().filter(f -> f != null).count(), cleared, created));
	}

	/** Whether any jump or call (not just fall-through) leads to a. */
	private boolean hasFlowInto(Address a) {
		for (Reference r : getReferencesTo(a)) {
			if (r.getReferenceType().isFlow()) {
				return true;
			}
		}
		return false;
	}

	private void helpers() throws Exception {
		Function common = null;
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			monitor.checkCancelled();
			for (var h : HELPERS.entrySet()) {
				if (startsWith(f.getEntryPoint(), h.getValue())) {
					nameFunction(f, h.getKey(), "Borland compiler helper, identified by its code bytes.");
					if (h.getKey().equals("__IOERROR")) {
						ioerrorGlobals(f);
					}
				}
			}
			if (startsWith(f.getEntryPoint(), LDIV_COMMON)) {
				common = f;
				nameFunction(f, "__ldivmod", "Shared body of LDIV@/LUDIV@/LMOD@/LUMOD@ (op in CX).");
			}
		}
		if (common == null) {
			return;
		}
		// Stubs: "XOR CX,CX" or "MOV CX,n" followed by a jump to the shared body.
		for (Function f : runtimeFunctions()) {
			Instruction first = getInstructionAt(f.getEntryPoint());
			Instruction second = first == null ? null : first.getNext();
			if (second == null || !second.getMnemonicString().equals("JMP") || second.getFlows().length != 1 ||
				!second.getFlows()[0].equals(common.getEntryPoint())) {
				continue;
			}
			String s = first.toString();
			int op = s.equals("XOR CX,CX") ? 0 : s.matches("MOV CX,0x[0-3]") ? Integer.decode(s.substring(7)) : -1;
			if (op >= 0) {
				nameFunction(f, LDIV_STUBS[op], "Entry stub into __ldivmod with CX=" + op + ".");
			}
		}
	}

	/** __IOerror stores into _doserrno first and errno second: "MOV word ptr [x],SI". */
	private void ioerrorGlobals(Function f) throws Exception {
		List<Long> stores = new ArrayList<>();
		for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
			if (ins.toString().matches("MOV word ptr \\[0x[0-9a-f]+\\],SI")) {
				stores.add(memOperand(ins));
			}
		}
		if (stores.size() == 2) {
			labelGlobal(stores.get(0), "_doserrno", "last DOS error code");
			labelGlobal(stores.get(1), "errno", "C error number");
		}
	}

	/** Names the unambiguous wrappers; returns them by AH. */
	private Map<Integer, Function> dosWrappers() throws Exception {
		Map<Integer, Function> result = new HashMap<>();
		Map<Integer, List<Function>> byAh = new HashMap<>();
		for (Function f : runtimeFunctions()) {
			if (f.getBody().getNumAddresses() > DOS_WRAPPER_MAX_BYTES) {
				continue;
			}
			int ints = 0, ah = -1;
			boolean retf = false;
			for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
				String s = ins.toString();
				if (s.equals("INT 0x21")) {
					ints++;
					ah = ahBefore(ins);
				}
				retf |= ins.getMnemonicString().equals("RETF");
			}
			// _exit never returns, so it has no RETF.
			if (ints == 1 && (retf || ah == 0x4c) && DOS.containsKey(ah)) {
				byAh.computeIfAbsent(ah, k -> new ArrayList<>()).add(f);
			}
		}
		for (var e : byAh.entrySet()) {
			int ah = e.getKey();
			if (e.getValue().size() == 1) {
				Function f = e.getValue().get(0);
				nameFunction(f, DOS.get(ah), String.format(
					"Thin wrapper around DOS INT 21h AH=%02Xh (%s); name presumed from that.", ah, DOS_DESC[ah]));
				result.put(ah, f);
			}
			else {
				println(String.format("WcRuntime: %d candidates for INT 21h AH=%02Xh, none named",
					e.getValue().size(), ah));
			}
		}
		return result;
	}

	/** AH loaded within the 8 instructions before an INT: "MOV AH,n" or "MOV AX,n". */
	private int ahBefore(Instruction ins) {
		Instruction p = ins;
		for (int i = 0; i < 8; i++) {
			p = p.getPrevious();
			if (p == null || p.getFlowType().isJump() || p.getFlowType().isCall()) {
				return -1;
			}
			String s = p.toString();
			if (s.startsWith("MOV AH,0x")) {
				return Integer.decode(s.substring(7));
			}
			if (s.startsWith("MOV AX,0x")) {
				return Integer.decode(s.substring(7)) >> 8;
			}
			if (s.startsWith("MOV AH,") || s.startsWith("MOV AX,")) {
				return -1;
			}
		}
		return -1;
	}

	private void setMainSignature(Function main) {
		try {
			DataType charPtrPtr = new PointerDataType(new PointerDataType(CharDataType.dataType, 2), 2);
			Parameter[] params = {
				new ParameterImpl("argc", IntegerDataType.dataType, currentProgram),
				new ParameterImpl("argv", charPtrPtr, currentProgram),
				new ParameterImpl("envp", charPtrPtr, currentProgram),
			};
			main.replaceParameters(Function.FunctionUpdateType.DYNAMIC_STORAGE_FORMAL_PARAMS, true,
				SourceType.ANALYSIS, params);
			main.setReturnType(IntegerDataType.dataType, SourceType.ANALYSIS);
		}
		catch (Exception e) {
			println("WcRuntime: could not set main's signature: " + e.getMessage());
		}
	}

	// ---- helpers ----

	/** Functions in code segment 0, where the startup code and the runtime library live. */
	private List<Function> runtimeFunctions() {
		List<Function> out = new ArrayList<>();
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			long e = f.getEntryPoint().getOffset();
			if (e >= rtlStart && e < rtlEnd && !f.isThunk()) {
				out.add(f);
			}
		}
		return out;
	}

	private boolean startsWith(Address a, Sig sig) {
		try {
			byte[] hash = MessageDigest.getInstance("SHA-256").digest(getBytes(a, sig.length()));
			return HexFormat.of().formatHex(hash).startsWith(sig.sha256());
		}
		catch (Exception e) {
			return false;
		}
	}

	private Function calledFunction(Instruction ins) {
		Function f = getFunctionAt(ins.getFlows()[0]);
		return f != null && f.isThunk() ? f.getThunkedFunction(true) : f;
	}

	private static boolean isPushMem(Instruction ins) {
		return ins != null && ins.toString().matches("PUSH word ptr \\[0x[0-9a-f]+\\]");
	}

	/** The displacement of an instruction's memory operand, e.g. 0x86 in "PUSH word ptr [0x86]". */
	private static Long memOperand(Instruction ins) {
		for (int i = 0; i < ins.getNumOperands(); i++) {
			for (Object o : ins.getOpObjects(i)) {
				if (o instanceof Scalar s && ins.getDefaultOperandRepresentation(i).contains("[")) {
					return s.getUnsignedValue();
				}
			}
		}
		return null;
	}

	private void nameFunction(Function f, String name, String why) throws Exception {
		if (f == null || f.getSymbol().getSource() != SourceType.DEFAULT) {
			return;
		}
		try {
			f.setName(name, SourceType.ANALYSIS);
		}
		catch (DuplicateNameException e) {
			println("WcRuntime: " + name + " is already used, not naming " + f.getEntryPoint());
			return;
		}
		String old = getPlateComment(f.getEntryPoint());
		setPlateComment(f.getEntryPoint(), (old == null ? "" : old + "\n") + "Borland runtime (WcRuntime): " + why);
		named.add(String.format("%-12s %s", name, f.getEntryPoint()));
	}

	private void labelGlobal(Long offset, String name, String what) throws Exception {
		if (offset == null) {
			return;
		}
		Address a = space.getAddress(dgroup, offset.intValue());
		var existing = getSymbolAt(a);
		if (existing != null && existing.getSource() != SourceType.DEFAULT) {
			return;
		}
		createLabel(a, name, true, SourceType.ANALYSIS);
		setEOLComment(a, what);
		named.add(String.format("%-12s %s (data)", name, a));
	}

	private Address entryPoint(JsonObject manifest, int load) {
		JsonObject entry = manifest.getAsJsonObject("entry");
		return space.getAddress(load + entry.get("cs").getAsInt(), entry.get("ip").getAsInt());
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
