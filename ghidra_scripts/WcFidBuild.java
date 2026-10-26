// Builds a Function ID database from the Borland Turbo C++ 1.0 runtime imported into this project:
// one library per top-level folder (an OMF .LIB, e.g. /CM.LIB/<module>) or top-level program
// (an .OBJ, e.g. /C0M.OBJ). Run it against any one program of the project; it walks the whole
// project itself. The database is registered with Ghidra only while it is being filled.
// Args: <output .fidb>
//@category WC

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

import ghidra.app.script.GhidraScript;
import ghidra.feature.fid.db.FidDB;
import ghidra.feature.fid.db.FidFile;
import ghidra.feature.fid.db.FidFileManager;
import ghidra.feature.fid.db.FidFilter;
import ghidra.feature.fid.service.FidPopulateResult;
import ghidra.feature.fid.service.FidService;
import ghidra.framework.model.DomainFile;
import ghidra.framework.model.DomainFolder;
import ghidra.program.model.listing.Program;

public class WcFidBuild extends GhidraScript {

	private static final String FAMILY = "Borland Turbo C++";
	private static final String VERSION = "1.0";

	@Override
	protected void run() throws Exception {
		File out = new File(getScriptArgs()[0]);
		if (out.exists() && !out.delete()) {
			throw new IllegalStateException("cannot replace " + out);
		}
		FidFileManager manager = FidFileManager.getInstance();
		manager.createNewFidDatabase(out);
		FidFile fidFile = manager.addUserFidFile(out);
		try {
			FidService service = new FidService();
			FidFilter filter = new FidFilter(currentProgram.getLanguageID().getIdAsString(), "", "");
			DomainFolder root = state.getProject().getProjectData().getRootFolder();

			// Library name -> its programs
			Map<String, List<DomainFile>> libraries = new java.util.TreeMap<>();
			for (DomainFolder folder : root.getFolders()) {
				List<DomainFile> programs = new ArrayList<>();
				collectPrograms(folder, programs);
				if (!programs.isEmpty()) {
					libraries.put(folder.getName(), programs);
				}
			}
			for (DomainFile file : root.getFiles()) {
				if (Program.class.isAssignableFrom(file.getDomainObjectClass())) {
					libraries.put(file.getName(), List.of(file));
				}
			}

			FidDB db = fidFile.getFidDB(true);
			try {
				for (var e : libraries.entrySet()) {
					FidPopulateResult result = service.createNewLibraryFromPrograms(db, FAMILY, VERSION, e.getKey(),
						e.getValue(), null, filter, null, null, monitor);
					println(String.format("WcFidBuild: %-12s %4d modules, %4d functions added, %4d excluded",
						e.getKey(), e.getValue().size(), result.getTotalAdded(), result.getTotalExcluded()));
				}
				db.saveDatabase("Saving", monitor);
			}
			finally {
				db.close();
			}
		}
		finally {
			manager.removeUserFile(fidFile);
		}
		println("WcFidBuild: wrote " + out);
	}

	private void collectPrograms(DomainFolder folder, List<DomainFile> out) {
		for (DomainFile f : folder.getFiles()) {
			if (Program.class.isAssignableFrom(f.getDomainObjectClass())) {
				out.add(f);
			}
		}
		for (DomainFolder sub : folder.getFolders()) {
			collectPrograms(sub, out);
		}
	}
}
