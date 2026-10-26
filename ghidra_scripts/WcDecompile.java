// Decompiles every function in WC_FLAT.EXE and writes C, one file per original segment
// (code_XXXX.c for resident code, ovlNN.c for overlays), plus functions.tsv as an index.
// Args: <manifest.json> <output dir>
//@category WC

import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import ghidra.app.decompiler.ClangCaseToken;
import ghidra.app.decompiler.ClangFieldToken;
import ghidra.app.decompiler.ClangFuncNameToken;
import ghidra.app.decompiler.ClangLabelToken;
import ghidra.app.decompiler.ClangLine;
import ghidra.app.decompiler.ClangToken;
import ghidra.app.decompiler.ClangTokenGroup;
import ghidra.app.decompiler.ClangTypeToken;
import ghidra.app.decompiler.ClangVariableToken;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.decompiler.PrettyPrinter;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.SegmentedAddress;
import ghidra.program.model.listing.Function;
import ghidra.program.model.pcode.PcodeOp;
import ghidra.program.model.symbol.IllegalCharCppTransformer;
import ghidra.program.model.symbol.NameTransformer;

public class WcDecompile extends GhidraScript {

	private static final int TIMEOUT_SECONDS = 120;

	private record Segment(int start, int end, String name, String kind) {}

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		JsonObject manifest = JsonParser.parseString(Files.readString(Path.of(args[0]))).getAsJsonObject();
		Path outDir = Path.of(args[1]);
		Files.createDirectories(outDir);
		int load = loadSegment(manifest);
		readTables(Path.of(args[0]).resolveSibling("switches.json"));

		List<Segment> segments = new ArrayList<>();
		for (JsonElement e : manifest.getAsJsonArray("segments")) {
			JsonObject s = e.getAsJsonObject();
			int start = (load + s.get("seg").getAsInt()) * 16;
			segments.add(new Segment(start, start + Math.max(s.get("size").getAsInt(), 1),
				s.get("name").getAsString(), s.get("kind").getAsString()));
		}

		DecompInterface ifc = new DecompInterface();
		DecompileOptions options = new DecompileOptions();
		options.grabFromProgram(currentProgram);
		ifc.setOptions(options);
		ifc.toggleCCode(true);
		ifc.setSimplificationStyle("decompile");
		if (!ifc.openProgram(currentProgram)) {
			throw new IllegalStateException("decompiler failed to start: " + ifc.getLastMessage());
		}

		Map<String, StringBuilder> files = new TreeMap<>();
		StringBuilder index = new StringBuilder("function\taddress\tfile\tstatus\n");
		int total = currentProgram.getFunctionManager().getFunctionCount();
		int done = 0, failed = 0, thunks = 0;
		monitor.initialize(total);
		try {
			for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
				monitor.checkCancelled();
				monitor.incrementProgress(1);
				// Overlay stubs (jmp far) and other thunks add nothing; calls show the target's name.
				if (f.isThunk()) {
					thunks++;
					continue;
				}
				Address entry = f.getEntryPoint();
				String file = fileFor(segments, entry) + ".c";
				StringBuilder sb = files.computeIfAbsent(file, k -> new StringBuilder());
				sb.append("/* ").append(f.getName()).append(" @ ").append(entry).append(" */\n");

				DecompileResults res = ifc.decompileFunction(f, TIMEOUT_SECONDS, monitor);
				String status;
				if (res.decompileCompleted() && res.getDecompiledFunction() != null) {
					sb.append(render(f, res)).append('\n');
					status = "ok";
				}
				else {
					sb.append("/* decompilation failed: ").append(res.getErrorMessage().trim()).append(" */\n\n");
					status = "failed";
					failed++;
				}
				index.append(f.getName()).append('\t').append(entry).append('\t').append(file).append('\t')
						.append(status).append('\n');
				if (++done % 100 == 0) {
					println(String.format("WcDecompile: %d/%d", done, total - thunks));
				}
			}
		}
		finally {
			ifc.dispose();
		}

		for (var e : files.entrySet()) {
			try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(outDir.resolve(e.getKey())))) {
				w.println("/* Decompiled from " + manifest.get("source").getAsString() + " by Ghidra. */");
				w.println("#include \"ghidra_types.h\"");
				w.println();
				w.print(e.getValue());
			}
		}
		Files.writeString(outDir.resolve("functions.tsv"), index);
		Files.writeString(outDir.resolve("ghidra_types.h"), TYPES_HEADER);
		println(String.format("WcDecompile: %d functions decompiled (%d failed, %d thunks skipped) into %d files in %s",
			done, failed, thunks, files.size(), outDir));
		println(String.format("WcDecompile: %d of %d recovered switches relabelled (%d case labels, %d left as addresses)",
			switchesRelabelled, tables.size(), casesRelabelled, casesUnmapped));
	}

	// ---- Case labels for the jump tables WcSwitches.java recovered ----
	//
	// A jump-table override only gives the decompiler the case targets, not how the index maps to
	// them, so it prints the switch on the loaded table entry and labels each case with the target's
	// linear address:  switch((undefined4)*(undefined2 *)(EXPR * 2 + 0x1e6)) { case 0x3505b: ...
	// From the table we know which indices lead to each target, so print switch(EXPR) with real case
	// values instead, folding Borland's "x - N" bias into them. Anything else prints unchanged.

	private static final Pattern BIAS = Pattern.compile("\\((.*) ([-+]) (0x[0-9a-f]+|[0-9]+)U?\\)");
	private static final Pattern DEREF = Pattern.compile("\\(\\(undefined4\\)\\*\\(undefined2 \\*\\)\\((.*)\\)\\)");
	private static final Pattern ARRAY = Pattern.compile("\\(\\(undefined4\\)\\(\\(undefined2 \\*\\)&?\\w+\\)\\[(.*)\\]\\)");
	private static final Pattern TABLE_BASE = Pattern.compile("0x[0-9a-f]+|[0-9]+|\\(char \\*\\)&?\\w+|&?\\w+");

	/**
	 * The index from the decompiler's rendering of the table load, or null if it isn't one of:
	 *   ((undefined4)*(undefined2 *)(INDEX * 2 + 0x1e6))     the plain form, constant in hex or decimal
	 *   ((undefined4)*(undefined2 *)((char *)s_x + 0x21 + INDEX * 2))   table offset shown relative to
	 *                                                        a data symbol at the same offset in DGROUP
	 *   ((undefined4)((undefined2 *)&DAT_2231_1116)[INDEX])  the same, as an array access
	 */
	private static String tableIndex(String expr) {
		Matcher a = ARRAY.matcher(expr);
		if (a.matches() && balanced(a.group(1))) {
			return "(" + a.group(1) + ")";
		}
		Matcher d = DEREF.matcher(expr);
		if (!d.matches() || !balanced(d.group(1))) {
			return null;
		}
		String index = null;
		for (String term : splitTopLevel(d.group(1), " + ")) {
			if (term.endsWith(" * 2") && index == null) {
				index = term.substring(0, term.length() - 4);
			}
			else if (!TABLE_BASE.matcher(term).matches()) {
				return null;
			}
		}
		return index;
	}

	private static boolean balanced(String s) {
		int depth = 0;
		for (char c : s.toCharArray()) {
			depth += c == '(' ? 1 : c == ')' ? -1 : 0;
			if (depth < 0) {
				return false;
			}
		}
		return depth == 0;
	}

	/** Splits on sep where it occurs outside parentheses and brackets. */
	private static List<String> splitTopLevel(String s, String sep) {
		List<String> parts = new ArrayList<>();
		int depth = 0, start = 0;
		for (int j = 0; j < s.length(); j++) {
			char c = s.charAt(j);
			depth += c == '(' || c == '[' ? 1 : c == ')' || c == ']' ? -1 : 0;
			if (depth == 0 && s.startsWith(sep, j)) {
				parts.add(s.substring(start, j));
				start = j + sep.length();
				j += sep.length() - 1;
			}
		}
		parts.add(s.substring(start));
		return parts;
	}

	/** Jump instruction (linear) -> case targets (linear) in index order. */
	private final Map<Long, List<Long>> tables = new HashMap<>();
	private int switchesRelabelled, casesRelabelled, casesUnmapped;

	private void readTables(Path path) throws Exception {
		if (!Files.exists(path)) {
			return;
		}
		for (JsonElement e : JsonParser.parseString(Files.readString(path)).getAsJsonArray()) {
			JsonObject t = e.getAsJsonObject();
			List<Long> targets = new ArrayList<>();
			t.getAsJsonArray("targets").forEach(v -> targets.add(v.getAsLong()));
			tables.put(t.get("jump").getAsLong(), targets);
		}
	}

	private String render(Function f, DecompileResults res) {
		ClangTokenGroup markup = res.getCCodeMarkup();
		if (markup == null || tables.isEmpty()) {
			return res.getDecompiledFunction().getC();
		}
		List<ClangLine> lines = new PrettyPrinter(f, markup, null).getLines();
		NameTransformer names = new IllegalCharCppTransformer(); // as DecompileResults.getDecompiledFunction()
		List<ClangToken> tokens = new ArrayList<>();
		List<Integer> lineOf = new ArrayList<>();
		for (int i = 0; i < lines.size(); i++) {
			for (ClangToken t : lines.get(i).getAllTokens()) {
				tokens.add(t);
				lineOf.add(i);
			}
		}

		Map<Long, Long> bias = new HashMap<>(); // relabelled switch (jump address) -> value of index 0
		StringBuilder out = new StringBuilder();
		StringBuilder line = new StringBuilder();
		int current = -1;
		for (int i = 0; i < tokens.size(); i++) {
			if (lineOf.get(i) != current) {
				if (current >= 0) {
					out.append(line).append('\n');
				}
				current = lineOf.get(i);
				line.setLength(0);
				line.append(lines.get(current).getIndentString());
			}
			ClangToken t = tokens.get(i);
			String text = tokenText(t, names);

			Long jump = recoveredSwitch(t);
			int open = nextText(tokens, i + 1);
			if (jump != null && open >= 0 && tokens.get(open).getText().equals("(")) {
				int close = matchingParen(tokens, open);
				if (close >= 0) {
					StringBuilder expr = new StringBuilder();
					for (int j = open; j <= close; j++) {
						expr.append(tokenText(tokens.get(j), names));
					}
					String index = tableIndex(expr.toString());
					if (index != null) {
						long base = 0;
						Matcher b = BIAS.matcher(index);
						if (b.matches()) {
							index = b.group(1);
							base = Long.decode(b.group(3)) * (b.group(2).equals("-") ? 1 : -1);
						}
						if (index.startsWith("(") && index.endsWith(")") && matchingParen(index) == index.length() - 1) {
							index = index.substring(1, index.length() - 1);
						}
						bias.put(jump, base);
						switchesRelabelled++;
						line.append("switch(").append(index).append(")");
						// If the original header wrapped, its rest (e.g. " {") joins this line.
						i = close;
						current = lineOf.get(close);
						continue;
					}
				}
			}

			if (t instanceof ClangCaseToken ct) {
				PcodeOp sw = ct.getSwitchOp();
				Long switchJump = sw == null ? null : sw.getSeqnum().getTarget().getOffset();
				if (switchJump != null && bias.containsKey(switchJump)) {
					// With an override, the label's value is the case target's linear address.
					long target = ct.getValue();
					List<Long> targets = tables.get(switchJump);
					List<String> values = new ArrayList<>();
					for (int k = 0; k < targets.size(); k++) {
						if (targets.get(k) == target) {
							values.add(caseValue(k + bias.get(switchJump)));
						}
					}
					if (values.isEmpty()) {
						casesUnmapped++;
					}
					else {
						casesRelabelled++;
						text = String.join(": case ", values);
					}
				}
			}
			line.append(text);
		}
		if (current >= 0) {
			out.append(line).append('\n');
		}
		return out.toString();
	}

	/** A token's text with names cleaned the way PrettyPrinter does it. */
	private static String tokenText(ClangToken t, NameTransformer names) {
		boolean isName = t instanceof ClangFuncNameToken || t instanceof ClangVariableToken ||
			t instanceof ClangTypeToken || t instanceof ClangFieldToken || t instanceof ClangLabelToken;
		return isName && t.getSyntaxType() != ClangToken.CONST_COLOR ? names.simplify(t.getText()) : t.getText();
	}

	/** For a "switch" keyword token, the jump address if its BRANCHIND is one of the recovered tables. */
	private Long recoveredSwitch(ClangToken t) {
		PcodeOp op = t.getPcodeOp();
		if (!t.getText().equals("switch") || op == null || op.getOpcode() != PcodeOp.BRANCHIND) {
			return null;
		}
		long jump = op.getSeqnum().getTarget().getOffset();
		return tables.containsKey(jump) ? jump : null;
	}

	/** Index of the next token at or after i with non-empty text, or -1. */
	private static int nextText(List<ClangToken> tokens, int i) {
		for (int j = i; j < tokens.size(); j++) {
			if (!tokens.get(j).getText().isEmpty()) {
				return j;
			}
		}
		return -1;
	}

	private static int matchingParen(List<ClangToken> tokens, int open) {
		int depth = 0;
		for (int j = open; j < tokens.size(); j++) {
			String s = tokens.get(j).getText();
			depth += s.equals("(") ? 1 : s.equals(")") ? -1 : 0;
			if (depth == 0) {
				return j;
			}
		}
		return -1;
	}

	private static int matchingParen(String s) {
		int depth = 0;
		for (int j = 0; j < s.length(); j++) {
			depth += s.charAt(j) == '(' ? 1 : s.charAt(j) == ')' ? -1 : 0;
			if (depth == 0) {
				return j;
			}
		}
		return -1;
	}

	private static String caseValue(long v) {
		return v >= 0 && v < 10 ? Long.toString(v) : (v < 0 ? "-0x" + Long.toHexString(-v) : "0x" + Long.toHexString(v));
	}

	private static String fileFor(List<Segment> segments, Address a) {
		long linear = a.getOffset();
		Segment best = null;
		for (Segment s : segments) {
			if ((s.kind.equals("code") || s.kind.equals("overlay")) && s.start <= linear && linear < s.end &&
				(best == null || s.start > best.start)) {
				best = s;
			}
		}
		return best != null ? best.name : "unknown";
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

	private static final String TYPES_HEADER = """
		/* Ghidra's built-in type names, for reading the decompiled 16-bit code. */
		#pragma once
		typedef unsigned char   undefined;
		typedef unsigned char   undefined1;
		typedef unsigned short  undefined2;
		typedef unsigned char   undefined3[3];
		typedef unsigned long   undefined4;
		typedef unsigned char   byte;
		typedef unsigned short  word;
		typedef unsigned long   dword;
		typedef unsigned char   uchar;
		typedef unsigned short  ushort;
		typedef unsigned int    uint;
		typedef unsigned long   ulong;
		typedef unsigned char   bool;
		typedef void            code;
		""";
}
