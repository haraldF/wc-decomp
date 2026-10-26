// Pre-analysis setup for WC_FLAT.EXE: pins DS to DGROUP in all code, seeds overlay entry
// points as named functions, and annotates them with their original overlay/stub location.
// Args: <manifest.json>
//@category WC

import java.math.BigInteger;
import java.nio.file.Files;
import java.nio.file.Path;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.SegmentedAddress;
import ghidra.program.model.address.SegmentedAddressSpace;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.ProgramContext;
import ghidra.program.model.symbol.SourceType;

public class WcSetup extends GhidraScript {

	@Override
	protected void run() throws Exception {
		JsonObject manifest = JsonParser.parseString(Files.readString(Path.of(getScriptArgs()[0])))
				.getAsJsonObject();
		SegmentedAddressSpace space =
			(SegmentedAddressSpace) currentProgram.getAddressFactory().getDefaultAddressSpace();
		int load = loadSegment(manifest);
		println("WcSetup: load segment " + Integer.toHexString(load));

		// Ghidra's switch analysis reads CS-relative jump tables from the wrong segment and
		// creates bogus code and function starts from them; WcSwitches.java does this instead.
		setAnalysisOption(currentProgram, "Decompiler Switch Analysis", "false");

		JsonElement dgroup = manifest.get("dgroup");
		if (dgroup != null && !dgroup.isJsonNull()) {
			Register ds = currentProgram.getRegister("DS");
			ProgramContext ctx = currentProgram.getProgramContext();
			BigInteger dsValue = BigInteger.valueOf(load + dgroup.getAsInt());
			for (JsonElement e : manifest.getAsJsonArray("segments")) {
				JsonObject s = e.getAsJsonObject();
				String kind = s.get("kind").getAsString();
				int size = s.get("size").getAsInt();
				if (!(kind.equals("code") || kind.equals("overlay")) || size == 0) {
					continue;
				}
				int seg = load + s.get("seg").getAsInt();
				Address start = space.getAddress(seg, s.get("minOff").getAsInt());
				Address end = space.getAddress(seg, size - 1);
				if (currentProgram.getMemory().contains(start) && start.compareTo(end) <= 0) {
					ctx.setValue(ds, start, end, dsValue);
				}
			}
			println("WcSetup: DS = " + dsValue.toString(16) + " in code segments");
		}

		int count = 0;
		for (JsonElement e : manifest.getAsJsonArray("overlays")) {
			JsonObject o = e.getAsJsonObject();
			int index = o.get("index").getAsInt();
			int seg = load + o.get("seg").getAsInt();
			int stubSeg = o.get("stubSeg").getAsInt();
			for (JsonElement ee : o.getAsJsonArray("entries")) {
				JsonObject entry = ee.getAsJsonObject();
				int target = entry.get("target").getAsInt();
				int stubOffset = entry.get("stubOffset").getAsInt();
				Address addr = space.getAddress(seg, target);
				Address stub = space.getAddress(load + stubSeg, stubOffset);
				String name = String.format("ovl%02d_%04x", index, target);

				disassemble(stub);
				disassemble(addr);
				Function f = getFunctionAt(addr);
				if (f == null) {
					f = createFunction(addr, name);
				}
				else if (f.getSymbol().getSource() == SourceType.DEFAULT) {
					f.setName(name, SourceType.ANALYSIS);
				}
				if (f != null) {
					setPlateComment(addr, String.format(
						"Overlay %d, offset %04x (original far-call target: stub %04x:%04x)",
						index, target, stubSeg, stubOffset));
					count++;
				}
			}
		}
		println("WcSetup: seeded " + count + " overlay entry points");
	}

	/** The segment the MZ loader placed the image at, derived from the program entry point. */
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
