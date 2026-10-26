// Applies the Borland Turbo C++ Function ID database (built by WcFidBuild.java) to WC_FLAT.EXE,
// the same way Ghidra's Function ID analyzer does, then turns the matched link names into C names
// (one leading underscore removed: _printf -> printf, __exit -> _exit). All-uppercase names are
// pascal-convention functions without the underscore and are left as they are (__IOERROR).
// The database is registered with Ghidra only while it is applied.
// Args: <.fidb>
//@category WC

import java.io.File;
import java.util.HashSet;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.feature.fid.cmd.ApplyFidEntriesCommand;
import ghidra.feature.fid.db.FidFile;
import ghidra.feature.fid.db.FidFileManager;
import ghidra.feature.fid.service.FidService;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;
import ghidra.util.exception.DuplicateNameException;

public class WcFid extends GhidraScript {

	@Override
	protected void run() throws Exception {
		File db = new File(getScriptArgs()[0]);
		if (!db.isFile()) {
			println("WcFid: no Function ID database at " + db + ", skipping");
			return;
		}
		Set<Address> unnamed = new HashSet<>();
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			if (f.getSymbol().getSource() == SourceType.DEFAULT) {
				unnamed.add(f.getEntryPoint());
			}
		}

		FidFileManager manager = FidFileManager.getInstance();
		FidFile fidFile = manager.addUserFidFile(db);
		if (fidFile == null) {
			throw new IllegalStateException("not a valid Function ID database: " + db);
		}
		try {
			FidService service = new FidService();
			ApplyFidEntriesCommand cmd = new ApplyFidEntriesCommand(currentProgram.getMemory(),
				service.getDefaultScoreThreshold(), service.getDefaultMultiNameThreshold(), false, true, false);
			cmd.applyTo(currentProgram, monitor);
		}
		finally {
			manager.removeUserFile(fidFile);
		}

		int named = 0, renamed = 0;
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			if (!unnamed.contains(f.getEntryPoint()) || f.getSymbol().getSource() == SourceType.DEFAULT) {
				continue;
			}
			named++;
			String link = f.getName();
			String c = cName(link);
			if (!c.equals(link)) {
				try {
					f.setName(c, SourceType.ANALYSIS);
					renamed++;
				}
				catch (DuplicateNameException e) {
					println("WcFid: kept " + link + " at " + f.getEntryPoint() + ", " + c + " is already used");
				}
			}
		}
		println(String.format("WcFid: Function ID named %d functions (%d converted to C names)", named, renamed));
	}

	/** Borland's C compiler prefixes cdecl names with "_"; pascal names are uppercase and unprefixed. */
	private static String cName(String link) {
		boolean pascal = link.equals(link.toUpperCase());
		return link.startsWith("_") && !pascal ? link.substring(1) : link;
	}
}
