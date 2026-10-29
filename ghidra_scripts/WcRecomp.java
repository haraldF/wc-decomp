// Statically recompiles WC_FLAT.EXE to C: every instruction's p-code becomes C operating on an
// emulated 8086 state (runtime/wc_rt.h): R[] holds Ghidra's register space, M[] 1 MB of memory.
//  - Each Ghidra function becomes "void F_<linear>(uint32_t at)", entered at any of its
//    instructions via a switch; branches inside it are gotos, anything else goes through run().
//  - Calls become C calls (the p-code still pushes the return address, and RET pops it).
//  - INT n becomes cpu_int(n); IN/OUT become port_in/port_out.
//  - Ghidra's 16-bit model sometimes sets CS to the instruction address rounded down to 64 KB;
//    those assignments get the instruction's real segment from the manifest instead.
//  - Like DOS, the runtime can load the program at any segment: values that came from MZ
//    relocations become "value + wc_reloc", and relocs.bin lists the relocated words in the image.
//    Code addresses in the generated C stay those of the 1000:0000 image (code_addr() maps).
// Writes <out>/seg_*.c (one per original segment), <out>/wc_dispatch.c, <out>/image.bin (the load
// image as relocated for 1000:0000) and <out>/relocs.bin (linear addresses of relocated words).
// Code reached only through pointers in data has no Ghidra function; list such entry points
// (linear, hex, one per line) in the optional entries file and they get recompiled too, along
// with any functions they call. The runtime reports such addresses ("npm run discover").
// Args: <manifest.json> <output dir> [entries file]
//   or:  driver <output dir> <name> <entries file>  for an image of code the program loads at
//        runtime (a driver), imported at a 64 KB-aligned virtual segment: it is position independent
//        (CS comes from the register), and writes drv_<name>.c, .h and _tables.c.
//@category WC

import java.io.ByteArrayOutputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import ghidra.app.script.GhidraScript;
import ghidra.app.util.PseudoDisassembler;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.SegmentedAddress;
import ghidra.program.model.address.SegmentedAddressSpace;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.pcode.PcodeOp;
import ghidra.program.model.pcode.Varnode;
import ghidra.program.model.symbol.Reference;

public class WcRecomp extends GhidraScript {

	private record Segment(int seg, long start, long end, String name, boolean overlay) {}

	private static final long ES = 0x100, CS = 0x102, SS = 0x104, DS = 0x106, SP = 0x10;
	private static final long IMAGE_BASE = 0x10000;

	private final List<Segment> segments = new ArrayList<>();
	private final Map<Long, String> entries = new HashMap<>(); // linear entry -> C function name
	private int csFixes, nearWrapFixes, fragmentSwitches, overrideFixes, relocConstants, farJumpFixes, iretFixes, codeReads, unhandled;
	/** whether the instruction being translated is in an overlay */
	private boolean inOverlay;
	/** unique offset -> segment register the segment() producing it used (after override fixes) */
	private final Map<Long, Long> segRegOf = new HashMap<>();
	/** unique offset -> address varnode it was loaded from, within the current instruction */
	private final Map<Long, Varnode> loadAddrOf = new HashMap<>();
	/** linear address of each relocated word -> its value (relocated for segment 1000) */
	private final TreeMap<Long, Integer> relocs = new TreeMap<>();
	/** relocated values within the instruction being translated */
	private Set<Long> relocValues = Set.of();

	/** Instructions before which the runtime's wc_hook(address) runs (script argument 4: hex, commas). */
	private final Set<Long> hooks = new HashSet<>();

	/** Driver mode: the image's virtual segment, or -1 for the program itself. */
	private int driverSeg = -1;
	/** linear address -> length of each emitted instruction (driver mode: for the byte ranges) */
	private final TreeMap<Long, Integer> codeBytes = new TreeMap<>();

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		if (args[0].equals("driver")) {
			runDriver(Path.of(args[1]), args[2], Path.of(args[3]));
			return;
		}
		JsonObject manifest = JsonParser.parseString(Files.readString(Path.of(args[0]))).getAsJsonObject();
		if (args.length > 3) {
			for (String h : args[3].split(",")) {
				if (!h.isBlank()) hooks.add(Long.parseLong(h.trim(), 16));
			}
		}
		Path out = Path.of(args[1]);
		Files.createDirectories(out);
		int load = loadSegment(manifest);
		if (load * 16L != IMAGE_BASE) {
			throw new IllegalStateException("expected the image at 1000:0000, not " + Integer.toHexString(load) + ":0000");
		}
		for (JsonElement e : manifest.getAsJsonArray("segments")) {
			JsonObject s = e.getAsJsonObject();
			String kind = s.get("kind").getAsString();
			if (kind.equals("code") || kind.equals("overlay") || kind.equals("stub")) {
				int seg = load + s.get("seg").getAsInt();
				segments.add(new Segment(seg, seg * 16L, seg * 16L + Math.max(s.get("size").getAsInt(), 1),
					s.get("name").getAsString(), kind.equals("overlay")));
			}
		}

		var relocIt = currentProgram.getRelocationTable().getRelocations();
		while (relocIt.hasNext()) {
			Address a = relocIt.next().getAddress();
			relocs.put(a.getOffset(), getShort(a) & 0xffff);
		}

		List<Function> functions = new ArrayList<>();
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			functions.add(f);
			entries.put(f.getEntryPoint().getOffset(), fname(f.getEntryPoint().getOffset()));
		}
		// Every overlay stub entry (a JMP FAR into its overlay) can be the target of a function
		// pointer, but Ghidra only made functions of those called directly.
		List<Long> seeds = new ArrayList<>();
		for (JsonElement e : manifest.getAsJsonArray("overlays")) {
			JsonObject o = e.getAsJsonObject();
			long stub = (load + o.get("stubSeg").getAsInt()) * 16L;
			for (JsonElement entry : o.getAsJsonArray("entries")) {
				seeds.add(stub + entry.getAsJsonObject().get("stubOffset").getAsInt());
			}
		}
		if (args.length > 2 && Files.exists(Path.of(args[2]))) {
			for (String line : Files.readAllLines(Path.of(args[2]))) {
				line = line.replaceAll("#.*", "").trim();
				if (!line.isEmpty()) {
					seeds.add(Long.parseLong(line, 16));
				}
			}
		}
		// Near calls and jumps that Ghidra sent 64 KB off: their real targets may not be decoded yet.
		for (Instruction ins : currentProgram.getListing().getInstructions(true)) {
			for (Address t : ins.getFlows()) {
				long real = nearTarget(ins, t);
				if (real != t.getOffset()) {
					seeds.add(real);
					println("WcRecomp: near " + ins.getMnemonicString() + " at " + ins.getAddress() + " goes to " +
						Long.toHexString(real) + ", not " + t);
				}
			}
		}
		// Jumps, calls and fall-throughs of Ghidra's functions that lead out of every function (e.g. a
		// function that ends right before the code it falls into): decode those too.
		int gaps = 0;
		for (Function f : functions) {
			for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
				List<Address> targets = new ArrayList<>();
				for (Address t : ins.getFlows()) {
					targets.add(linearToAddress(nearTarget(ins, t)));
				}
				if (ins.getFallThrough() != null) {
					targets.add(ins.getFallThrough());
				}
				for (long t : switchTargets(ins)) {
					targets.add(linearToAddress(t));
				}
				for (Address t : targets) {
					if (!recompiled(t) && currentProgram.getMemory().contains(t)) {
						seeds.add(t.getOffset());
						gaps++;
					}
				}
			}
		}
		println("WcRecomp: " + gaps + " targets of functions outside every function");
		nearWrapFixes = 0;
		List<Fragment> fragments = fragments(seeds);
		for (Fragment fr : fragments) {
			entries.put(fr.entry, fname(fr.entry));
		}

		Map<String, StringBuilder> files = new TreeMap<>();
		// linear address of every emitted instruction -> C function, for run()
		TreeMap<Long, String> owner = new TreeMap<>();
		monitor.initialize(functions.size());
		for (Function f : functions) {
			monitor.checkCancelled();
			monitor.incrementProgress(1);
			List<Instruction> body = new ArrayList<>();
			for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
				body.add(ins);
			}
			long entry = f.getEntryPoint().getOffset();
			emitFunction(fname(entry), f.getName() + " @ " + f.getEntryPoint(), body, fileFor(entry, files), owner);
		}
		for (Fragment fr : fragments) {
			emitFunction(fname(fr.entry), "discovered entry point @ " + fr.body.get(0).getAddress(), fr.body,
				fileFor(fr.entry, files), owner);
		}

		for (var e : files.entrySet()) {
			writeIfChanged(out.resolve(e.getKey()), withPrototypes(e.getValue().toString()));
		}
		// The build compiles every file here: remove those of segments that no longer have code.
		try (var old = Files.newDirectoryStream(out, "seg_*.c")) {
			for (Path p : old) {
				if (!files.containsKey(p.getFileName().toString())) {
					Files.delete(p);
				}
			}
		}
		writeHeaderAndDispatch(out, owner, manifest);
		writeImage(out);
		writeRelocs(out);
		println(String.format("WcRecomp: %d functions + %d discovered fragments, %d instructions in %d files; " +
			"%d CS fixes, %d near wrap fixes, %d segment override fixes, %d far jump fixes, %d IRET fixes, %d overlay code reads, " +
			"%d relocated constants, %d unhandled p-code ops", functions.size(), fragments.size(), owner.size(),
			files.size(), csFixes, nearWrapFixes, overrideFixes, farJumpFixes, iretFixes, codeReads, relocConstants, unhandled));
	}

	private void runDriver(Path out, String name, Path entriesFile) throws Exception {
		Files.createDirectories(out);
		Address start = currentProgram.getMinAddress();
		driverSeg = ((SegmentedAddress) start).getSegment();
		long virt = driverSeg * 16L;
		List<Function> functions = new ArrayList<>();
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			functions.add(f);
			entries.put(f.getEntryPoint().getOffset(), fname(f.getEntryPoint().getOffset()));
		}
		List<Long> seeds = new ArrayList<>();
		if (Files.exists(entriesFile)) {
			for (String line : Files.readAllLines(entriesFile)) {
				line = line.replaceAll("#.*", "").trim();
				if (!line.isEmpty() && Long.parseLong(line, 16) - virt < 0x10000 && Long.parseLong(line, 16) >= virt) {
					seeds.add(Long.parseLong(line, 16));
				}
			}
		}
		// Origin's drivers start with a zero word and a table of their entry points (offsets), ended by
		// a small value (0022h in the graphics drivers, 0100h in the music driver); the game calls
		// them through far pointers it builds from that table.
		int tableEntries = 0;
		if ((getShort(start) & 0xffff) == 0) {
			for (int i = 2; i < 0x100; i += 2) {
				int off = getShort(start.add(i)) & 0xffff;
				if (off < 0x200) {
					break;
				}
				seeds.add(virt + off);
				tableEntries++;
			}
		}
		println("WcRecomp: driver " + name + ": " + tableEntries + " entry points in its table");
		for (Function f : functions) {
			for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
				for (long t : switchTargets(ins)) {
					if (!recompiled(linearToAddress(t))) {
						seeds.add(t);
					}
				}
			}
		}
		// Callbacks: "mov word cs:[var],imm16" into a variable that "call/jmp cs:[var]" goes through,
		// found in the decoded code (the 64 KB snapshot also holds whatever followed the driver), until
		// no new ones turn up.
		List<Fragment> fragments = new ArrayList<>();
		Set<Long> callbacks = new HashSet<>();
		for (int round = 0; round < 8 && !seeds.isEmpty(); round++) {
			for (Fragment fr : fragments(seeds)) {
				fragments.add(fr);
				entries.put(fr.entry, fname(fr.entry));
			}
			List<Instruction> decoded = new ArrayList<>();
			for (Function f : functions) {
				currentProgram.getListing().getInstructions(f.getBody(), true).forEach(decoded::add);
			}
			fragments.forEach(fr -> decoded.addAll(fr.body));
			seeds = new ArrayList<>();
			List<Long> found = callbackTargets(decoded, virt);
			found.addAll(registerTableTargets(decoded, virt));
			for (long t : found) {
				if (callbacks.add(t)) {
					seeds.add(t);
				}
			}
		}
		println("WcRecomp: driver " + name + ": " + callbacks.size() + " callbacks and register-indexed table entries");

		String h = "drv_" + name + ".h";
		StringBuilder sb = new StringBuilder("#include \"wc_rt.h\"\n#include \"" + h + "\"\n\n");
		TreeMap<Long, String> owner = new TreeMap<>();
		for (Function f : functions) {
			List<Instruction> body = new ArrayList<>();
			for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
				body.add(ins);
			}
			emitFunction(fname(f.getEntryPoint().getOffset()), f.getName() + " @ " + f.getEntryPoint(), body, sb, owner);
		}
		for (Fragment fr : fragments) {
			emitFunction(fname(fr.entry), "discovered entry point @ " + fr.body.get(0).getAddress(), fr.body, sb, owner);
		}
		writeIfChanged(out.resolve("drv_" + name + ".c"), sb.toString());

		StringBuilder hdr = new StringBuilder("/* Generated by WcRecomp.java. */\n#pragma once\n#include <stdint.h>\n\n");
		for (String fn : new TreeSet<>(entries.values())) {
			hdr.append("void ").append(fn).append("(uint32_t at);\n");
		}
		writeIfChanged(out.resolve(h), hdr.toString());

		// tables: code addresses, the instruction byte ranges and the image, for wc_drivers.c
		StringBuilder t = new StringBuilder("/* Generated by WcRecomp.java: driver " + name + ". */\n");
		t.append("#include \"wc_rt.h\"\n#include \"").append(h).append("\"\n\n");
		t.append("static const uint32_t addr[] = {\n");
		for (long a : owner.keySet()) t.append("0x").append(Long.toHexString(a)).append(",\n");
		t.append("};\nstatic void (*const func[])(uint32_t) = {\n");
		for (String fn : owner.values()) t.append(fn).append(",\n");
		t.append("};\nstatic const uint16_t ranges[][2] = {\n");
		List<long[]> merged = new ArrayList<>();
		for (var e : codeBytes.entrySet()) {
			long off = e.getKey() - virt, len = e.getValue();
			long[] last = merged.isEmpty() ? null : merged.get(merged.size() - 1);
			if (last != null && last[0] + last[1] >= off) {
				last[1] = Math.max(last[1], off + len - last[0]);
			}
			else {
				merged.add(new long[] { off, len });
			}
		}
		for (long[] r : merged) {
			t.append("{ 0x").append(Long.toHexString(r[0])).append(", 0x").append(Long.toHexString(r[1])).append(" },\n");
		}
		byte[] image = new byte[0x10000];
		currentProgram.getMemory().getBytes(start, image);
		t.append("};\nstatic const uint8_t image[0x10000] = {");
		for (int i = 0; i < image.length; i++) {
			t.append(i % 32 == 0 ? "\n" : "").append(image[i] & 0xff).append(',');
		}
		t.append("\n};\n\nconst struct wc_driver wc_drv_").append(name).append(" = {\n\t\"").append(name).append("\", 0x")
				.append(Long.toHexString(virt)).append(", ").append(owner.size()).append(", addr, func, ")
				.append(merged.size()).append(", ranges, image,\n};\n");
		writeIfChanged(out.resolve("drv_" + name + "_tables.c"), t.toString());
		println(String.format("WcRecomp: driver %s: %d functions + %d fragments, %d instructions in %d byte ranges; " +
			"%d unhandled p-code ops", name, functions.size(), fragments.size(), owner.size(), merged.size(), unhandled));
	}

	/** Unchanged files keep their timestamps, so the C build only recompiles what changed. */
	private static void writeIfChanged(Path file, String content) throws Exception {
		if (!Files.exists(file) || !Files.readString(file).equals(content)) {
			Files.writeString(file, content);
		}
	}

	private record Fragment(long entry, List<Instruction> body) {}

	/**
	 * Code reached only through pointers in data (stub entries, the entries file) is decoded with the
	 * pseudo-disassembler, which leaves Ghidra's listing alone: there, such code is often missing or
	 * decoded misaligned (e.g. the assembly overlay manager mixes code and data). Each entry point
	 * becomes a fragment holding the instructions its control flow reaches, up to code that is
	 * already recompiled; the functions it calls become fragments too.
	 */
	private List<Fragment> fragments(List<Long> seeds) throws Exception {
		List<Address> pending = new ArrayList<>();
		for (long seed : seeds) {
			pending.add(linearToAddress(seed));
		}
		PseudoDisassembler pdis = new PseudoDisassembler(currentProgram);
		Set<Long> done = new HashSet<>();
		List<Fragment> result = new ArrayList<>();
		while (!pending.isEmpty()) {
			Address a = pending.remove(pending.size() - 1);
			long entry = a.getOffset();
			if (entries.containsKey(entry) || !done.add(entry) || recompiled(a)) {
				continue;
			}
			TreeMap<Long, Instruction> body = new TreeMap<>();
			List<Address> work = new ArrayList<>(List.of(a));
			while (!work.isEmpty() && body.size() < 20000) {
				Address x = work.remove(work.size() - 1);
				if (body.containsKey(x.getOffset()) || x.getOffset() != entry && recompiled(x)) {
					continue;
				}
				Instruction pi;
				try {
					pi = pdis.disassemble(x);
				}
				catch (Exception e) {
					pi = null;
				}
				if (pi == null) {
					continue;
				}
				body.put(x.getOffset(), pi);
				for (Address t : pi.getFlows()) {
					long real = nearTarget(pi, t);
					(pi.getFlowType().isCall() ? pending : work).add(real == t.getOffset() ? t : linearToAddress(real));
				}
				if (pi.getFallThrough() != null) {
					work.add(pi.getFallThrough());
				}
				for (long t : switchTargets(pi)) {
					work.add(linearToAddress(t));
				}
			}
			if (body.isEmpty()) {
				println("WcRecomp: cannot decode an instruction at " + a);
				continue;
			}
			result.add(new Fragment(entry, new ArrayList<>(body.values())));
		}
		println("WcRecomp: decoded " + result.size() + " fragments from " + seeds.size() + " entry points, " +
			fragmentSwitches + " switch tables in them");
		return result;
	}

	/**
	 * The cases of a Borland switch in code Ghidra didn't analyze (WcSwitches only sees its functions):
	 * "cmp bx,N; ja default; shl bx,1; jmp cs:[bx+table]", the table in the code segment. In the
	 * (hand-written) drivers also unbounded "jmp cs:[bx/si/di+table]": as many words as look like code
	 * offsets (their tables end in zeros).
	 */
	private List<Long> switchTargets(Instruction ins) {
		List<Long> targets = new ArrayList<>();
		try {
			byte[] b = ins.getBytes();
			List<Long> sparse = sparseSwitchTargets(ins, b);
			if (!sparse.isEmpty()) {
				fragmentSwitches++;
				return sparse;
			}
			int modrm = b.length == 5 ? b[2] & 0xff : 0;
			if (b.length != 5 || (b[0] & 0xff) != 0x2e || (b[1] & 0xff) != 0xff ||
					!(modrm == 0xa7 || driverSeg >= 0 && (modrm == 0xa4 || modrm == 0xa5))) {
				return targets;
			}
			long at = ins.getAddress().getOffset();
			int table = (b[3] & 0xff) | (b[4] & 0xff) << 8;
			byte[] p = new byte[8];
			currentProgram.getMemory().getBytes(linearToAddress(at - 8), p);
			int n;
			if ((p[1] & 0xff) == 0x83 && (p[2] & 0xff) == 0xfb && (p[4] & 0xff) == 0x77 && (p[6] & 0xff) == 0xd1 && (p[7] & 0xff) == 0xe3) {
				n = p[3] & 0xff;
			}
			else if ((p[0] & 0xff) == 0x81 && (p[1] & 0xff) == 0xfb && (p[4] & 0xff) == 0x77 && (p[6] & 0xff) == 0xd1 && (p[7] & 0xff) == 0xe3) {
				n = (p[2] & 0xff) | (p[3] & 0xff) << 8;
			}
			else if (driverSeg >= 0) {
				n = -1;
			}
			else {
				return targets;
			}
			long cs = driverSeg >= 0 ? driverSeg * 16L : realSegment(ins) * 16L;
			for (int i = 0; (n < 0 || i <= n) && i < 256; i++) {
				int off = getShort(linearToAddress(cs + table + 2 * i)) & 0xffff;
				if (n < 0 && (off < 0x200 || off >= 0xc000)) {
					break;
				}
				targets.add(cs + off);
			}
			fragmentSwitches++;
		}
		catch (Exception e) {
			// not readable: no cases
		}
		return targets;
	}

	/**
	 * Tables reached as "add/mov reg,table; call/jmp cs:[reg]" (reg = SI, DI, BX): the words from the
	 * table up to the next decoded instruction (in the drivers the code continues right after it).
	 */
	private List<Long> registerTableTargets(List<Instruction> code, long base) throws Exception {
		Set<Long> starts = new HashSet<>();
		for (Instruction ins : code) {
			starts.add(ins.getAddress().getOffset());
		}
		List<Long> targets = new ArrayList<>();
		for (Instruction ins : code) {
			byte[] b = ins.getBytes();
			int m = b.length == 3 ? b[2] & 0xff : 0;
			if (b.length != 3 || (b[0] & 0xff) != 0x2e || (b[1] & 0xff) != 0xff ||
					!(m == 0x14 || m == 0x15 || m == 0x17 || m == 0x24 || m == 0x25 || m == 0x27)) {
				continue;
			}
			int reg = m & 7; // 4 SI, 5 DI, 7 BX
			int addOp = reg == 4 ? 0xc6 : reg == 5 ? 0xc7 : 0xc3, movOp = reg == 4 ? 0xbe : reg == 5 ? 0xbf : 0xbb;
			long at = ins.getAddress().getOffset();
			byte[] p = new byte[4];
			currentProgram.getMemory().getBytes(linearToAddress(at - 4), p);
			int table;
			if ((p[0] & 0xff) == 0x81 && (p[1] & 0xff) == addOp) {
				table = (p[2] & 0xff) | (p[3] & 0xff) << 8;
			}
			else if ((p[1] & 0xff) == movOp) {
				table = (p[2] & 0xff) | (p[3] & 0xff) << 8;
			}
			else {
				continue;
			}
			for (int i = 0; i < 16 && !starts.contains(base + table + 2 * i); i++) {
				int off = getShort(linearToAddress(base + table + 2 * i)) & 0xffff;
				if (off < 0x200 || off >= 0xc000) {
					break;
				}
				targets.add(base + off);
			}
		}
		return targets;
	}

	/** Constants stored with "mov word cs:[var],imm16" into variables that "call/jmp cs:[var]" uses. */
	private static List<Long> callbackTargets(List<Instruction> code, long base) {
		Set<Integer> vars = new HashSet<>();
		List<int[]> stores = new ArrayList<>();
		for (Instruction ins : code) {
			try {
				byte[] b = ins.getBytes();
				if (b.length == 5 && (b[0] & 0xff) == 0x2e && (b[1] & 0xff) == 0xff &&
						((b[2] & 0xff) == 0x16 || (b[2] & 0xff) == 0x26)) {
					vars.add((b[3] & 0xff) | (b[4] & 0xff) << 8);
				}
				if (b.length == 7 && (b[0] & 0xff) == 0x2e && (b[1] & 0xff) == 0xc7 && (b[2] & 0xff) == 0x06) {
					stores.add(new int[] { (b[3] & 0xff) | (b[4] & 0xff) << 8, (b[5] & 0xff) | (b[6] & 0xff) << 8 });
				}
			}
			catch (Exception e) {
				// no bytes
			}
		}
		// "mov reg,imm16" shortly before "mov cs:[var],reg" (AX: 2e a3 var; others: 2e 89 modrm var)
		List<Instruction> sorted = new ArrayList<>(code);
		sorted.sort((x, y) -> x.getAddress().compareTo(y.getAddress()));
		for (int k = 0; k < sorted.size(); k++) {
			try {
				byte[] b = sorted.get(k).getBytes();
				int reg, var;
				if (b.length == 4 && (b[0] & 0xff) == 0x2e && (b[1] & 0xff) == 0xa3) {
					reg = 0;
					var = (b[2] & 0xff) | (b[3] & 0xff) << 8;
				}
				else if (b.length == 5 && (b[0] & 0xff) == 0x2e && (b[1] & 0xff) == 0x89 && ((b[2] & 0xff) & 0xc7) == 0x06) {
					reg = ((b[2] & 0xff) >> 3) & 7;
					var = (b[3] & 0xff) | (b[4] & 0xff) << 8;
				}
				else {
					continue;
				}
				for (int j = Math.max(0, k - 4); j < k; j++) {
					byte[] m = sorted.get(j).getBytes();
					if (m.length == 3 && (m[0] & 0xff) == 0xb8 + reg) {
						stores.add(new int[] { var, (m[1] & 0xff) | (m[2] & 0xff) << 8 });
					}
				}
			}
			catch (Exception e) {
				// no bytes
			}
		}
		List<Long> targets = new ArrayList<>();
		for (int[] st : stores) {
			if (vars.contains(st[0]) && st[1] >= 0x200) {
				targets.add(base + st[1]);
			}
		}
		return targets;
	}

	/**
	 * Borland's switch for scattered case values: "mov cx,N; mov bx,table; L: mov ax,cs:[bx];
	 * cmp ax,value (or directly cmp cs:[bx],ax); jz found; inc bx; inc bx; loop L; jmp default; found: jmp cs:[bx+2N]" with N
	 * values and then N targets in the code segment.
	 */
	private List<Long> sparseSwitchTargets(Instruction ins, byte[] b) throws Exception {
		List<Long> targets = new ArrayList<>();
		int disp;
		if (b.length == 4 && (b[0] & 0xff) == 0x2e && (b[1] & 0xff) == 0xff && (b[2] & 0xff) == 0x67) {
			disp = b[3] & 0xff;
		}
		else if (b.length == 5 && (b[0] & 0xff) == 0x2e && (b[1] & 0xff) == 0xff && (b[2] & 0xff) == 0xa7) {
			disp = (b[3] & 0xff) | (b[4] & 0xff) << 8;
		}
		else {
			return targets;
		}
		long at = ins.getAddress().getOffset();
		byte[] p = new byte[40];
		currentProgram.getMemory().getBytes(linearToAddress(at - p.length), p);
		for (int i = p.length - 9; i >= 0; i--) {
			int op = p[i + 7] & 0xff; // mov ax,cs:[bx] / cmp cs:[bx],ax / cmp ax,cs:[bx]
			if ((p[i] & 0xff) == 0xb9 && (p[i + 3] & 0xff) == 0xbb && (p[i + 6] & 0xff) == 0x2e &&
					(op == 0x8b || op == 0x39 || op == 0x3b) && (p[i + 8] & 0xff) == 0x07) {
				int n = (p[i + 1] & 0xff) | (p[i + 2] & 0xff) << 8;
				int table = (p[i + 4] & 0xff) | (p[i + 5] & 0xff) << 8;
				if (disp != 2 * n || n > 256) {
					return targets;
				}
				long cs = driverSeg >= 0 ? driverSeg * 16L : realSegment(ins) * 16L;
				for (int k = 0; k < n; k++) {
					targets.add(cs + (getShort(linearToAddress(cs + table + disp + 2 * k)) & 0xffff));
				}
				return targets;
			}
		}
		return targets;
	}

	/** An instruction start inside a Ghidra function, i.e. code that is emitted anyway. */
	private boolean recompiled(Address a) {
		return getInstructionAt(a) != null && getFunctionContaining(a) != null;
	}

	private StringBuilder fileFor(long entry, Map<String, StringBuilder> files) {
		Segment seg = segmentOf(entry);
		String file = "seg_" + (seg != null ? seg.name : "unknown") + ".c";
		return files.computeIfAbsent(file, k -> new StringBuilder());
	}

	/** A linear address as seg:off in the segment the manifest puts it in. */
	private Address linearToAddress(long linear) {
		Segment s = segmentOf(linear);
		int seg = driverSeg >= 0 ? driverSeg : s != null ? s.seg : (int) (linear >> 4);
		SegmentedAddressSpace space = (SegmentedAddressSpace) currentProgram.getAddressFactory().getDefaultAddressSpace();
		return space.getAddress(seg, (int) (linear - seg * 16L));
	}

	// ---- per function ----

	private void emitFunction(String fn, String title, List<Instruction> body, StringBuilder sb,
			TreeMap<Long, String> owner) {
		Set<Long> labels = new HashSet<>();
		for (Instruction ins : body) {
			labels.add(ins.getAddress().getOffset());
			owner.putIfAbsent(ins.getAddress().getOffset(), fn);
			codeBytes.put(ins.getAddress().getOffset(), ins.getLength());
		}
		Set<String> uniques = new TreeSet<>();
		StringBuilder code = new StringBuilder();
		for (int i = 0; i < body.size(); i++) {
			Instruction ins = body.get(i);
			Instruction next = i + 1 < body.size() ? body.get(i + 1) : null;
			emitInstruction(ins, next, labels, uniques, code);
		}

		sb.append("/* ").append(title).append(" */\n");
		sb.append("void ").append(fn).append("(uint32_t at)\n{\n");
		if (!uniques.isEmpty()) {
			sb.append("\tuint64_t ").append(String.join(" = 0, ", uniques)).append(" = 0;\n");
		}
		sb.append("\tswitch (at) {\n");
		for (Instruction ins : body) {
			long a = ins.getAddress().getOffset();
			sb.append("\tcase 0x").append(Long.toHexString(a)).append(": goto L_").append(Long.toHexString(a)).append(";\n");
		}
		sb.append("\tdefault: cpu_fail(\"no instruction in ").append(fn).append(" at\", at);\n\t}\n");
		sb.append(code);
		sb.append("}\n\n");
	}

	private void emitInstruction(Instruction ins, Instruction next, Set<Long> labels, Set<String> uniques,
			StringBuilder c) {
		long at = ins.getAddress().getOffset();
		int realCs = realSegment(ins);
		c.append("L_").append(Long.toHexString(at)).append(": TRACE_INS(0x").append(Long.toHexString(at))
				.append("); /* ").append(ins.getAddress()).append(" ").append(ins.toString().replace("*/", "* /"))
				.append(" */\n");
		if (hooks.contains(at)) {
			c.append("\twc_hook(0x").append(Long.toHexString(at)).append(");\n");
		}

		PcodeOp[] ops = ins.getPcode(false);
		boolean relative = false;
		for (PcodeOp op : ops) {
			if ((op.getOpcode() == PcodeOp.BRANCH || op.getOpcode() == PcodeOp.CBRANCH) && op.getInput(0).isConstant()) {
				relative = true;
			}
		}
		Long override = segmentOverride(ins);
		loadAddrOf.clear();
		segRegOf.clear();
		Segment own = driverSeg >= 0 ? null : segmentOf(at);
		inOverlay = own != null && own.overlay;
		relocValues = new HashSet<>();
		for (var r : relocs.subMap(at, at + ins.getLength()).values()) {
			relocValues.add((long) r);
		}
		Set<Long> swiOutputs = new HashSet<>();
		Map<Long, Varnode> segOfUnique = new HashMap<>(); // unique offset -> segment input of segment()
		boolean ends = false;
		for (int i = 0; i < ops.length; i++) {
			PcodeOp op = ops[i];
			if (relative) {
				c.append("P_").append(Long.toHexString(at)).append("_").append(i).append(":;\n");
			}
			ends = false;
			String line = translate(op, i, ins, realCs, override, labels, uniques, swiOutputs, segOfUnique);
			c.append('\t').append(line).append('\n');
			int opc = op.getOpcode();
			ends = opc == PcodeOp.BRANCH || opc == PcodeOp.BRANCHIND || opc == PcodeOp.RETURN;
		}
		if (relative) {
			c.append("P_").append(Long.toHexString(at)).append("_").append(ops.length).append(":;\n");
		}

		// Continue with the next instruction unless the p-code already left.
		if (!ends) {
			Address ft = ins.getFallThrough();
			if (ft == null) {
				// e.g. a call to a function that never returns
				c.append("\tcpu_fail(\"no fall-through after\", 0x").append(Long.toHexString(at)).append(");\n");
			}
			else if (next == null || next.getAddress().getOffset() != ft.getOffset()) {
				c.append('\t').append(jump(ft.getOffset(), labels)).append('\n');
			}
		}
	}

	private String translate(PcodeOp op, int index, Instruction ins, int realCs, Long override,
			Set<Long> labels, Set<String> uniques, Set<Long> swiOutputs, Map<Long, Varnode> segOfUnique) {
		long at = ins.getAddress().getOffset();
		Varnode out = op.getOutput();
		Varnode[] in = op.getInputs();
		int os = out != null ? out.getSize() : 0;
		String a = in.length > 0 ? val(in[0], uniques) : null;
		String b = in.length > 1 ? val(in[1], uniques) : null;
		int as = in.length > 0 ? in[0].getSize() : 0;
		switch (op.getOpcode()) {
			case PcodeOp.COPY:
				trackLoad(out, in[0]);
				if (out.isRegister() && out.getOffset() == CS && in[0].isUnique() && driverSeg >= 0) {
					return "/* CS stays: position-independent code */";
				}
				if (out.isRegister() && out.getOffset() == CS && in[0].isUnique()) {
					csFixes++;
					return set(out, "0x" + Integer.toHexString(realCs) + " + wc_reloc", uniques) + " /* real CS */";
				}
				return set(out, a, uniques);
			case PcodeOp.LOAD: {
				if (out.isUnique()) {
					loadAddrOf.put(out.getOffset(), in[1]);
				}
				// Overlay code reads its own constants (switch tables) through CS. At runtime the heap
				// overlaps the flattened overlays' addresses, so read them from the pristine image.
				Long segReg = in[1].isUnique() ? segRegOf.get(in[1].getOffset()) : null;
				if (inOverlay && segReg != null && segReg == CS) {
					codeReads++;
					return set(out, "mrd_code(" + val(in[1], uniques) + ", " + os + ")", uniques);
				}
				return set(out, "mrd(" + val(in[1], uniques) + ", " + os + ")", uniques);
			}
			case PcodeOp.STORE:
				return "mwr(" + b + ", " + in[2].getSize() + ", " + val(in[2], uniques) + ");";
			case PcodeOp.INT_ADD: return set(out, a + " + " + b, uniques);
			case PcodeOp.INT_SUB: return set(out, a + " - " + b, uniques);
			case PcodeOp.INT_MULT: return set(out, a + " * " + b, uniques);
			case PcodeOp.INT_AND: return set(out, a + " & " + b, uniques);
			case PcodeOp.INT_OR: return set(out, a + " | " + b, uniques);
			case PcodeOp.INT_XOR: return set(out, a + " ^ " + b, uniques);
			case PcodeOp.INT_DIV: return set(out, "udiv(" + a + ", " + b + ", 0x" + Long.toHexString(at) + ")", uniques);
			case PcodeOp.INT_REM: return set(out, "urem(" + a + ", " + b + ", 0x" + Long.toHexString(at) + ")", uniques);
			case PcodeOp.INT_SDIV:
				return set(out, "sdiv(" + a + ", " + b + ", " + as + ", 0x" + Long.toHexString(at) + ")", uniques);
			case PcodeOp.INT_SREM:
				return set(out, "srem(" + a + ", " + b + ", " + as + ", 0x" + Long.toHexString(at) + ")", uniques);
			case PcodeOp.INT_LEFT: return set(out, "shl(" + a + ", " + b + ")", uniques);
			case PcodeOp.INT_RIGHT: return set(out, "shr(" + a + ", " + b + ")", uniques);
			case PcodeOp.INT_SRIGHT: return set(out, "sar(" + a + ", " + b + ", " + as + ")", uniques);
			case PcodeOp.INT_2COMP: return set(out, "0 - " + a, uniques);
			case PcodeOp.INT_NEGATE: return set(out, "~" + a, uniques);
			case PcodeOp.INT_EQUAL: return set(out, "(" + a + " == " + b + ")", uniques);
			case PcodeOp.INT_NOTEQUAL: return set(out, "(" + a + " != " + b + ")", uniques);
			case PcodeOp.INT_LESS: return set(out, "(" + a + " < " + b + ")", uniques);
			case PcodeOp.INT_LESSEQUAL: return set(out, "(" + a + " <= " + b + ")", uniques);
			case PcodeOp.INT_SLESS: return set(out, "(sx(" + a + ", " + as + ") < sx(" + b + ", " + as + "))", uniques);
			case PcodeOp.INT_SLESSEQUAL:
				return set(out, "(sx(" + a + ", " + as + ") <= sx(" + b + ", " + as + "))", uniques);
			case PcodeOp.INT_CARRY: return set(out, "carry(" + a + ", " + b + ", " + as + ")", uniques);
			case PcodeOp.INT_SCARRY: return set(out, "scarry(" + a + ", " + b + ", " + as + ")", uniques);
			case PcodeOp.INT_SBORROW: return set(out, "sborrow(" + a + ", " + b + ", " + as + ")", uniques);
			case PcodeOp.INT_ZEXT:
				trackLoad(out, in[0]);
				return set(out, a, uniques);
			case PcodeOp.INT_SEXT: return set(out, "(uint64_t)sx(" + a + ", " + as + ")", uniques);
			case PcodeOp.BOOL_NEGATE: return set(out, "!" + a, uniques);
			case PcodeOp.BOOL_AND: return set(out, "(" + a + " && " + b + ")", uniques);
			case PcodeOp.BOOL_OR: return set(out, "(" + a + " || " + b + ")", uniques);
			case PcodeOp.BOOL_XOR: return set(out, "((" + a + " != 0) != (" + b + " != 0))", uniques);
			case PcodeOp.POPCOUNT: return set(out, "(uint64_t)__builtin_popcountll(" + a + ")", uniques);
			case PcodeOp.SUBPIECE: return set(out, "shr(" + a + ", " + (in[1].getOffset() * 8) + ")", uniques);
			case PcodeOp.PIECE:
				return set(out, "shl(" + a + ", " + (in[1].getSize() * 8) + ") | " + b, uniques);

			case PcodeOp.CALLOTHER: {
				String name = currentProgram.getLanguage().getUserDefinedOpName((int) in[0].getOffset());
				switch (name) {
					case "segment": {
						if (out.isUnique()) {
							segOfUnique.put(out.getOffset(), in[1]);
						}
						String segValue = val(in[1], uniques);
						Long segReg = in[1].isRegister() ? in[1].getOffset() : null;
						if (override != null && isDefaultOperandSegment(in[1], in[2]) && in[1].getOffset() != override) {
							overrideFixes++;
							segValue = "rd(0x" + Long.toHexString(override) + ", 2) /* segment override */";
							segReg = override;
						}
						if (out.isUnique() && segReg != null) {
							segRegOf.put(out.getOffset(), segReg);
						}
						return set(out, "lin(" + segValue + ", " + val(in[2], uniques) + ")", uniques);
					}
					case "swi":
						if (out != null && out.isUnique()) {
							swiOutputs.add(out.getOffset());
						}
						return "cpu_int(" + val(in[1], uniques) + ", 0x" + Long.toHexString(at) + ", 0x" +
							Long.toHexString(at + ins.getLength()) + ");";
					case "in":
						return set(out, "port_in(" + val(in[1], uniques) + ", " + os + ")", uniques);
					case "out":
						return "port_out(" + val(in[1], uniques) + ", " + val(in[2], uniques) + ", " + in[2].getSize() + ");";
					case "LOCK":
					case "UNLOCK":
						return "/* " + name + " */";
					default:
						unhandled++;
						return "cpu_fail(\"unhandled CALLOTHER " + name + " at\", 0x" + Long.toHexString(at) + ");";
				}
			}

			case PcodeOp.BRANCH:
				if (in[0].isConstant()) {
					return "goto P_" + Long.toHexString(at) + "_" + (index + (int) in[0].getOffset()) + ";";
				}
				return jump(nearTarget(ins, in[0].getAddress()), labels);
			case PcodeOp.CBRANCH: {
				String target = in[0].isConstant()
						? "goto P_" + Long.toHexString(at) + "_" + (index + (int) in[0].getOffset()) + ";"
						: jump(nearTarget(ins, in[0].getAddress()), labels);
				return "if (" + b + ") " + target;
			}
			case PcodeOp.BRANCHIND:
				if (ins.getMnemonicString().equals("JMPF") && in[0].isUnique() && loadAddrOf.containsKey(in[0].getOffset())) {
					// Ghidra models "JMP FAR m16:16" as a near jump to the offset word; load the far pointer.
					farJumpFixes++;
					String addr = val(loadAddrOf.get(in[0].getOffset()), uniques);
					return "{ uint64_t fa = " + addr + "; uint64_t fseg = mrd(fa + 2, 2); wr(0x102, 2, fseg); " +
						switchJump(ins, "lin(fseg, mrd(fa, 2))", labels) + " }";
				}
				return farCs(in[0], segOfUnique, uniques) + switchJump(ins, a, labels);
			case PcodeOp.CALL: {
				long t = nearTarget(ins, in[0].getAddress());
				String call = entries.containsKey(t) ? entries.get(t) + "(0x" + Long.toHexString(t) + ");"
						: "run_code(0x" + Long.toHexString(t) + ");";
				return "push_ret(0x" + Long.toHexString(at + ins.getLength()) + "); " + call + " pop_ret();";
			}
			case PcodeOp.CALLIND:
				if (in[0].isUnique() && swiOutputs.contains(in[0].getOffset())) {
					return "/* interrupt handled by cpu_int */";
				}
				return farCs(in[0], segOfUnique, uniques) + "push_ret(0x" + Long.toHexString(at + ins.getLength()) +
					"); run((uint32_t)" + a + "); pop_ret();";
			case PcodeOp.RETURN:
				if (ins.getMnemonicString().equals("IRET")) {
					// Ghidra's IRET pops the flags into the flags word only; unpack them into CF, IF, ...
					iretFixes++;
					return "set_flags_word(rd(0x280, 2)); return;";
				}
				return "return;";
			default:
				unhandled++;
				return "cpu_fail(\"unhandled p-code " + op.getMnemonic() + " at\", 0x" + Long.toHexString(at) + ");";
		}
	}

	/**
	 * Ghidra's 16-bit x86 model drops the segment override prefix for some instruction forms
	 * (e.g. "CALLF ES:[BX+2]" reads through DS). The prefix from the instruction bytes.
	 */
	private static Long segmentOverride(Instruction ins) {
		try {
			for (byte b : ins.getBytes()) {
				switch (b & 0xff) {
					case 0x26: return ES;
					case 0x2e: return CS;
					case 0x36: return SS;
					case 0x3e: return DS;
					case 0xf0: case 0xf2: case 0xf3: continue; // LOCK, REPNE, REP
					default: return null;
				}
			}
		}
		catch (Exception e) {
			// no bytes
		}
		return null;
	}

	/** A memory operand's default segment (DS, or SS for BP-based); stack accesses use SS:SP. */
	private static boolean isDefaultOperandSegment(Varnode seg, Varnode offset) {
		if (!seg.isRegister()) {
			return false;
		}
		if (seg.getOffset() == DS) {
			return true;
		}
		return seg.getOffset() == SS && !(offset.isRegister() && offset.getOffset() == SP);
	}

	private void trackLoad(Varnode out, Varnode in) {
		if (out != null && out.isUnique() && in.isUnique() && loadAddrOf.containsKey(in.getOffset())) {
			loadAddrOf.put(out.getOffset(), loadAddrOf.get(in.getOffset()));
		}
	}

	/** An indirect far transfer computes its target with segment(seg, off): load CS with seg first. */
	private String farCs(Varnode target, Map<Long, Varnode> segOfUnique, Set<String> uniques) {
		if (!target.isUnique()) {
			return "";
		}
		Varnode seg = segOfUnique.get(target.getOffset());
		if (seg == null || seg.isRegister() && seg.getOffset() == CS) {
			return "";
		}
		return "wr(0x102, 2, " + val(seg, uniques) + "); ";
	}

	/**
	 * The target of a relative near CALL/JMP/Jcc: the next instruction plus the signed displacement.
	 * Ghidra wraps the displacement within its own CS (the address rounded down to 64 KB), so a call
	 * across that boundary (e.g. from 2fd2:0554 back to 2fd2:0047) lands 64 KB off. Ghidra's target
	 * still has the right offset modulo 64 KB, so the displacement can be recovered from it.
	 */
	private long nearTarget(Instruction ins, Address target) {
		long t = target.getOffset();
		if (driverSeg >= 0 || !isRelative(ins)) {
			return t;
		}
		long next = ins.getAddress().getOffset() + ins.getLength();
		long real = next + (short) ((t - next) & 0xffff);
		if (real != t) {
			nearWrapFixes++;
		}
		return real;
	}

	/** CALL rel16, JMP rel16/rel8, Jcc rel8, LOOP/JCXZ (after any prefixes). */
	private static boolean isRelative(Instruction ins) {
		try {
			for (byte b : ins.getBytes()) {
				int op = b & 0xff;
				if (op == 0x26 || op == 0x2e || op == 0x36 || op == 0x3e || op == 0xf0 || op == 0xf2 || op == 0xf3) {
					continue;
				}
				return op == 0xe8 || op == 0xe9 || op == 0xeb || (op >= 0x70 && op <= 0x7f) || (op >= 0xe0 && op <= 0xe3);
			}
		}
		catch (Exception e) {
			// no bytes
		}
		return false;
	}

	/** Indirect jump: known targets in this function become gotos, anything else goes through run(). */
	private String switchJump(Instruction ins, String target, Set<Long> labels) {
		StringBuilder sb = new StringBuilder("switch (code_addr(" + target + ")) {");
		Set<Long> seen = new HashSet<>();
		for (Reference r : getReferencesFrom(ins.getAddress())) {
			long t = r.getToAddress().getOffset();
			if (r.getReferenceType().isJump() && labels.contains(t) && seen.add(t)) {
				sb.append(" case 0x").append(Long.toHexString(t)).append(": goto L_").append(Long.toHexString(t)).append(";");
			}
		}
		// A jump to the address the current call is expected to return to is a return (e.g. Borland's
		// _setargv saves its return address and later jumps through it).
		return sb.append(" default: if (is_return(code_addr(").append(target).append("))) return; run((uint32_t)")
				.append(target).append("); return; }").toString();
	}

	private String jump(long target, Set<Long> labels) {
		return labels.contains(target) ? "goto L_" + Long.toHexString(target) + ";"
				: "{ run_code(0x" + Long.toHexString(target) + "); return; }";
	}

	// ---- varnodes ----

	private String val(Varnode v, Set<String> uniques) {
		int s = v.getSize();
		if (v.isConstant()) {
			if (s == 2 && relocValues.contains(v.getOffset())) {
				relocConstants++;
				return "((0x" + Long.toHexString(v.getOffset()) + "ULL + wc_reloc) & 0xffff)";
			}
			return "0x" + Long.toHexString(v.getOffset() & mask(s)) + "ULL";
		}
		if (v.isRegister()) {
			return "rd(0x" + Long.toHexString(v.getOffset()) + ", " + s + ")";
		}
		if (v.isUnique()) {
			String u = "u" + Long.toHexString(v.getOffset());
			uniques.add(u);
			return s >= 8 ? u : "(" + u + " & 0x" + Long.toHexString(mask(s)) + "ULL)";
		}
		if (v.isAddress()) {
			return "mrd(0x" + Long.toHexString(v.getOffset()) + ", " + s + ")";
		}
		unhandled++;
		return "0 /* unhandled varnode " + v + " */";
	}

	private String set(Varnode out, String expr, Set<String> uniques) {
		int s = out.getSize();
		if (out.isRegister()) {
			return "wr(0x" + Long.toHexString(out.getOffset()) + ", " + s + ", " + expr + ");";
		}
		if (out.isUnique()) {
			String u = "u" + Long.toHexString(out.getOffset());
			uniques.add(u);
			return u + " = (uint64_t)(" + expr + ")" + (s >= 8 ? "" : " & 0x" + Long.toHexString(mask(s)) + "ULL") + ";";
		}
		if (out.isAddress()) {
			return "mwr(0x" + Long.toHexString(out.getOffset()) + ", " + s + ", " + expr + ");";
		}
		unhandled++;
		return "/* unhandled output " + out + " */";
	}

	private static long mask(int size) {
		return size >= 8 ? -1L : (1L << (size * 8)) - 1;
	}

	private static String fname(long linear) {
		return "F_" + Long.toHexString(linear);
	}

	// ---- whole program ----

	/** A generated file with the prototypes of the functions it uses, not all of wc_gen.h: that gains
	 * entries in every discover round, and a file then only changes when its own code does. */
	private static String withPrototypes(String body) {
		TreeSet<String> used = new TreeSet<>();
		Matcher m = FUNCTION_NAME.matcher(body);
		while (m.find()) {
			used.add(m.group());
		}
		StringBuilder sb = new StringBuilder("#include \"wc_rt.h\"\n\n");
		for (String fn : used) {
			sb.append("void ").append(fn).append("(uint32_t at);\n");
		}
		return sb.append("\n").append(body).toString();
	}

	private static final Pattern FUNCTION_NAME = Pattern.compile("\\bF_[0-9a-f]+\\b");

	private void writeHeaderAndDispatch(Path out, TreeMap<Long, String> owner, JsonObject manifest) throws Exception {
		StringBuilder h = new StringBuilder("/* Generated by WcRecomp.java. */\n#pragma once\n#include <stdint.h>\n\n");
		for (String fn : new TreeSet<>(entries.values())) {
			h.append("void ").append(fn).append("(uint32_t at);\n");
		}
		writeIfChanged(out.resolve("wc_gen.h"), h.toString());

		StringBuilder d = new StringBuilder("/* Generated by WcRecomp.java: instruction address -> function. */\n");
		d.append("#include \"wc_rt.h\"\n#include \"wc_gen.h\"\n\n");
		d.append("const uint32_t wc_code_count = ").append(owner.size()).append(";\n");
		d.append("const uint32_t wc_code_addr[] = {\n");
		for (long a : owner.keySet()) {
			d.append("0x").append(Long.toHexString(a)).append(",\n");
		}
		d.append("};\nvoid (*const wc_code_func[])(uint32_t) = {\n");
		for (String fn : owner.values()) {
			d.append(fn).append(",\n");
		}
		d.append("};\n");
		// Overlay code always runs with CS = its segment; the rest of the program is the original image.
		// Anything else is code the program loaded at runtime.
		List<Integer> ovl = new ArrayList<>();
		for (JsonElement e : manifest.getAsJsonArray("overlays")) {
			ovl.add(e.getAsJsonObject().get("seg").getAsInt());
		}
		d.append("const uint32_t wc_overlay_count = ").append(ovl.size()).append(";\n");
		d.append("const uint16_t wc_overlay_seg[] = {");
		for (int seg : ovl) {
			d.append(" 0x").append(Integer.toHexString(seg)).append(",");
		}
		d.append(" };\n");
		d.append("const uint32_t wc_program_size = 0x").append(Integer.toHexString(ovl.stream().min(Integer::compare)
				.orElse(0) * 16)).append(";\n");
		writeIfChanged(out.resolve("wc_dispatch.c"), d.toString());
	}

	/** The relocated image as Ghidra loaded it, from linear 0x10000. */
	private void writeImage(Path out) throws Exception {
		long end = IMAGE_BASE;
		for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
			if (b.isInitialized() && b.getStart() instanceof SegmentedAddress) {
				end = Math.max(end, b.getEnd().getOffset() + 1);
			}
		}
		byte[] image = new byte[(int) (end - IMAGE_BASE)];
		for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
			if (b.isInitialized() && b.getStart() instanceof SegmentedAddress) {
				byte[] bytes = new byte[(int) b.getSize()];
				b.getBytes(b.getStart(), bytes);
				System.arraycopy(bytes, 0, image, (int) (b.getStart().getOffset() - IMAGE_BASE), bytes.length);
			}
		}
		Files.write(out.resolve("image.bin"), image);
	}

	private void writeRelocs(Path out) throws Exception {
		java.nio.ByteBuffer buf = java.nio.ByteBuffer.allocate(relocs.size() * 4).order(java.nio.ByteOrder.LITTLE_ENDIAN);
		for (long a : relocs.keySet()) {
			buf.putInt((int) a);
		}
		Files.write(out.resolve("relocs.bin"), buf.array());
	}

	/**
	 * The segment an instruction runs in. Segments can overlap by a few bytes (e.g. the end of
	 * segment 0000 and the start of 03be), so Ghidra's segment for the instruction wins when it is
	 * one of the program's segments; otherwise the manifest's innermost segment.
	 */
	private int realSegment(Instruction ins) {
		long at = ins.getAddress().getOffset();
		if (ins.getAddress() instanceof SegmentedAddress sa) {
			for (Segment s : segments) {
				if (s.seg == sa.getSegment() && s.start <= at && at < s.end) {
					return s.seg;
				}
			}
		}
		Segment seg = segmentOf(at);
		return seg != null ? seg.seg : (int) (at >> 4);
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
