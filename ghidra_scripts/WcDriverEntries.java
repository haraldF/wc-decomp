// Before analysis of a captured driver image: disassemble and create functions at the entry
// points from the entries file that fall into this image (linear hex addresses, one per line).
// Args: <entries file>
//@category WC

import java.nio.file.Files;
import java.nio.file.Path;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.SegmentedAddress;
import ghidra.program.model.address.SegmentedAddressSpace;

public class WcDriverEntries extends GhidraScript {
	@Override
	protected void run() throws Exception {
		Path file = Path.of(getScriptArgs()[0]);
		if (!Files.exists(file)) {
			return;
		}
		// The image is a 64 KB snapshot: the driver plus whatever heap data follows it. Only decode
		// code reachable from the observed entry points; the instruction bytes identify the driver at
		// runtime, so they must not include heap data that differs from run to run.
		setAnalysisOption(currentProgram, "Function Start Search", "false");
		int seg = ((SegmentedAddress) currentProgram.getMinAddress()).getSegment();
		SegmentedAddressSpace space = (SegmentedAddressSpace) currentProgram.getAddressFactory().getDefaultAddressSpace();
		int n = 0;
		for (String line : Files.readAllLines(file)) {
			line = line.replaceAll("#.*", "").trim();
			if (line.isEmpty()) {
				continue;
			}
			long off = Long.parseLong(line, 16) - seg * 16L;
			if (off < 0 || off >= 0x10000) {
				continue;
			}
			Address a = space.getAddress(seg, (int) off);
			disassemble(a);
			if (getFunctionAt(a) == null && createFunction(a, null) != null) {
				n++;
			}
		}
		println("WcDriverEntries: created " + n + " functions");
	}
}
