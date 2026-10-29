// Usage: node src/cli.ts <command> [--game wc|sm2|wc2] [--exe file] [--borland dir] [--out dir]
//   --game       which program: wc (WC.EXE: Vega and Secret Missions 1, in out/, binary out/bin/wc) or
//                sm2 (SM2.EXE: Secret Missions 2, in out/sm2/, binary out/sm2/bin/sm2) or
//                wc2 (WC2.EXE: Wing Commander II in ../game2, in out/wc2/, binary out/wc2/bin/wc2)
//   flatten      write out/WC_FLAT.EXE (overlays folded in) and out/manifest.json
//   fid          build out/borland-tc10.fidb, a Function ID database of the Borland Turbo C++ 1.0
//                runtime, from CM.LIB, C0M.OBJ and OVERLAY.LIB in --borland (only once)
//   decompile    flatten, import into a Ghidra project, analyze, and write C to out/c/
//   redecompile  re-run only the C export on the existing Ghidra project (after you rename/retype in the GUI)
//   recomp       translate the analyzed program to compilable C (out/recomp/) and build out/bin/wc
//   build        only (re)build out/bin/wc from out/recomp/ and runtime/ (CMake, in out/build/)
//   web          build the web page in out/web/ from out/recomp/ and runtime/ (Emscripten, in out/build-web/)
//   discover     run out/bin/wc (each run for --seconds, default 60; WC_KEYS etc. from the environment);
//                when it reaches code that wasn't recompiled (reached only through pointers in data),
//                add that entry point to entries.txt, recompile, repeat;
//                when it calls code the game loaded at runtime (drivers), capture it to out/loaded/
//                and recompile that too
//   play         the same in the window, for playing: after each recompile the game starts again
//                (arguments after "play" go to the game)

import { spawn, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { appendFileSync, existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { parseArgs } from "node:util";
import { flatten, type Manifest } from "./flatten.ts";
import { findGhidra, runHeadless } from "./ghidra.ts";

const root = path.resolve(import.meta.dirname, "..");
const { positionals, values } = parseArgs({
  allowPositionals: true,
  options: {
    game: { type: "string", default: "wc" },
    exe: { type: "string" },
    borland: { type: "string", default: path.resolve(root, "../tc/extracted") },
    out: { type: "string", default: path.join(root, "out") },
    seconds: { type: "string", default: "60" },
  },
});

// The programs this project recompiles, each the GOG release's (by SHA-256): other releases differ
// in code and data, so the addresses in the entries file, the runtime and the tools would be wrong.
// rates: the flight routine that turns the steering rates (-9..9) into turning speeds, as a code
// address (image at 1000:0000), right after the input is read; roll: the roll rate's DS offset.
// The runtime sets the roll rate there from a flight stick's twist. cycles: the default speed
// (instructions per ms; Wing Commander paces animation by CPU speed).
// dir: the game folder (by default; --exe names another). rates 0: not found yet, no twist.
interface Game { exe: string; dir: string; sha256: string; out: string; entries: string; rates: number; roll: number; cycles: number }
const GAMES: Record<string, Game> = {
  // Wing Commander with the Secret Missions 1 (GOG)
  wc: { exe: "WC.EXE", dir: "../game", sha256: "c37d085909175abc95b4a51ffc884913c7d4ea52ba66ca932619ce1f67fa91ca", out: "", entries: "entries.txt",
    rates: 0x1482e /* 13be:0c4e */, roll: 0xc3ae, cycles: 8334 },
  // The Secret Missions 2: Crusade, a program of its own
  sm2: { exe: "SM2.EXE", dir: "../game", sha256: "900f379b04c7b6f11ae08cf84ca278b9260df8ed81d038cdb3907bbf136cd84d", out: "sm2", entries: "entries-sm2.txt",
    rates: 0x147b8 /* 13bb:0c08 */, roll: 0xc694, cycles: 8334 },
  // Wing Commander II: Vengeance of the Kilrathi (GOG), a new engine (Borland C++ 2.0)
  wc2: { exe: "WC2.EXE", dir: "../game2", sha256: "0eb4e54e97a29266b31ec73a149aa5f4a1a32e6b1a1aea74d287f0bfd890393f", out: "wc2",
    entries: "entries-wc2.txt", rates: 0, roll: 0, cycles: 14400 /* 8334 felt sluggish */ },
};
const gameName = values.game!;
const game = GAMES[gameName];
if (!game) {
  console.error(`unknown --game ${gameName} (${Object.keys(GAMES).join(", ")})`);
  process.exit(2);
}
const exePath = path.resolve(values.exe ?? path.join(root, game.dir, game.exe));

const command = positionals[0];
const baseOut = path.resolve(values.out);
const out = path.join(baseOut, game.out);
const flatExe = path.join(out, "WC_FLAT.EXE");
const manifestPath = path.join(out, "manifest.json");
const projectDir = path.join(out, "ghidra");
const cDir = path.join(out, "c");
const scriptPath = path.join(root, "ghidra_scripts");
const projectName = "WC";
const recompDir = path.join(out, "recomp");
const entriesFile = path.join(root, game.entries); // kept in the repository

// Code the game loads at runtime, captured from memory and recompiled at a virtual segment each.
interface Driver { name: string; seg: number; file: string }
const loadedDir = path.join(out, "loaded");
const capturesDir = path.join(loadedDir, "captures");
const driversFile = path.join(loadedDir, "drivers.json");
const readDrivers = (): Driver[] => (existsSync(driversFile) ? JSON.parse(readFileSync(driversFile, "utf8")) : []);
// Which virtual segment each driver gets, by a hash of its first 1 KB (kept in the repository, since
// the driver entry points in the entries files depend on it, and captures come in any order; shared
// by the programs, which load the same drivers).
const driverIdsFile = path.join(root, "drivers.txt");
const driverHash = (image: Buffer): string => createHash("sha256").update(image.subarray(0, 1024)).digest("hex").slice(0, 16);
const readDriverIds = (): { name: string; hash: string }[] =>
  (existsSync(driverIdsFile) ? readFileSync(driverIdsFile, "utf8").split("\n") : [])
    .map((l) => l.replace(/#.*/, "").trim().split(/\s+/))
    .filter((w) => w.length >= 2)
    .map(([name, hash]) => ({ name: name!, hash: hash! }));
const segHex = (n: number): string => n.toString(16).padStart(4, "0");

const importDriver = (d: Driver): Promise<void> =>
  runHeadless(findGhidra(), [
    projectDir, `${projectName}/loaded`,
    "-import", path.join(loadedDir, d.file), "-overwrite",
    "-loader", "BinaryLoader", "-loader-baseAddr", `${segHex(d.seg)}:0000`, "-processor", "x86:LE:16:Real Mode",
    "-scriptPath", scriptPath, "-preScript", "WcDriverEntries.java", entriesFile,
  ]);

/** wc_drivers.c: the runtime's list of recompiled drivers. */
function writeDriverList(drivers: Driver[]): void {
  const lines = ['/* Generated by cli.ts: the recompiled drivers. */', '#include "wc_rt.h"', ""];
  for (const d of drivers) lines.push(`extern const struct wc_driver wc_drv_${d.name};`);
  const items = drivers.map((d) => `&wc_drv_${d.name}`);
  lines.push(`const struct wc_driver *const wc_drivers[${Math.max(drivers.length, 1)}] = { ${items.join(", ") || "0"} };`);
  lines.push(`const uint32_t wc_driver_count = ${drivers.length};`, "");
  const file = path.join(recompDir, "wc_drivers.c");
  const text = lines.join("\n");
  if (!existsSync(file) || readFileSync(file, "utf8") !== text) writeFileSync(file, text);
}

const wcExe = path.join(out, "bin", gameName);
const gameDir = path.dirname(exePath);
const doRecomp = async (): Promise<void> => {
  mkdirSync(recompDir, { recursive: true });
  await runHeadless(findGhidra(), [
    projectDir, projectName,
    "-process", path.basename(flatExe), "-noanalysis", "-readOnly",
    "-scriptPath", scriptPath,
    "-postScript", "WcRecomp.java", manifestPath, recompDir, entriesFile, game.rates ? game.rates.toString(16) : "",
  ]);
  const drivers = readDrivers();
  for (const d of drivers) {
    await runHeadless(findGhidra(), [
      projectDir, `${projectName}/loaded`,
      "-process", d.file, "-noanalysis", "-readOnly",
      "-scriptPath", scriptPath,
      "-postScript", "WcRecomp.java", "driver", recompDir, d.name, entriesFile,
    ]);
  }
  writeDriverList(drivers);
  await doBuild();
};
/** Configure (once) and build with CMake; web: with Emscripten, into out/web. */
const cmakeBuild = async (web: boolean): Promise<void> => {
  writeProgram();
  const buildDir = path.join(out, web ? "build-web" : "build");
  const binDir = web ? path.join(baseOut, "web") : path.join(out, "bin"); // one page for both programs
  if (!existsSync(path.join(buildDir, "CMakeCache.txt"))) {
    const ninja = spawnSync("ninja", ["--version"]).status === 0 ? ["-G", "Ninja"] : [];
    const args = ["-S", root, "-B", buildDir, ...ninja, `-DWC_RECOMP_DIR=${recompDir}`, `-DWC_BIN_DIR=${binDir}`,
      `-DWC_NAME=${gameName}`];
    await (web ? exec("emcmake", ["cmake", ...args]) : exec("cmake", args));
  }
  await exec("cmake", ["--build", buildDir]);
  console.log(`built ${web ? path.join(binDir, "index.html") : wcExe}`);
};
const doBuild = (): Promise<void> => cmakeBuild(false);
const borlandProjectDir = path.join(baseOut, "borland");
const fidb = path.join(baseOut, "borland-tc10.fidb");
// Medium memory model runtime, startup code and overlay manager, as linked into WC.EXE.
const borlandFiles = ["CM.LIB", "C0M.OBJ", "OVERLAY.LIB"];

/** wc_program.c: what the runtime needs from the program's EXE header and name. */
function writeProgram(): void {
  const m = JSON.parse(readFileSync(manifestPath, "utf8")) as Manifest;
  const h = (n: number): string => `0x${n.toString(16).padStart(4, "0")}`;
  const text = [
    "/* Generated by cli.ts: the program's start, from its EXE header. */",
    '#include "wc_rt.h"', "",
    `const struct wc_program wc_program = { "${game.exe}", ${h(m.entry.cs)}, ${h(m.entry.ip)}, ${h(m.stack.ss)}, ${h(m.stack.sp)},`,
    `\t0x${game.rates.toString(16)}, ${h(game.roll)}, ${game.cycles} };`,
    "",
  ].join("\n");
  const file = path.join(recompDir, "wc_program.c");
  if (!existsSync(file) || readFileSync(file, "utf8") !== text) writeFileSync(file, text);
}

function doFlatten(): void {
  const exe = exePath;
  const file = readFileSync(exe);
  const sha = createHash("sha256").update(file).digest("hex");
  if (sha !== game.sha256) {
    throw new Error(`${exe} is not the expected ${game.exe} (SHA-256 ${sha}, expected ${game.sha256}); ` +
      "this project needs the GOG release's");
  }
  const { exe: flat, manifest } = flatten(file, exe);
  mkdirSync(out, { recursive: true });
  writeFileSync(flatExe, flat);
  writeFileSync(manifestPath, JSON.stringify(manifest, null, 2));
  const entries = manifest.overlays.reduce((n, o) => n + o.entries.length, 0);
  console.log(`${flatExe}: ${flat.length} bytes, ${manifest.overlays.length} overlays / ${entries} overlay entry points folded in`);
}

async function main(): Promise<void> {
  switch (command) {
    case "flatten":
      doFlatten();
      break;
    case "fid": {
      const dir = path.resolve(values.borland);
      const files = borlandFiles.map((f) => path.join(dir, f)).filter((f) => existsSync(f));
      if (!files.some((f) => f.endsWith("CM.LIB"))) {
        throw new Error(`no CM.LIB in ${dir} (extract it from MLIB.ZIP of Turbo C++ 1.0, or pass --borland)`);
      }
      mkdirSync(borlandProjectDir, { recursive: true });
      const ghidra = findGhidra();
      await runHeadless(ghidra, [
        borlandProjectDir, "Borland",
        "-import", ...files, "-recursive", "-overwrite", "-processor", "x86:LE:16:Real Mode",
      ]);
      // The build script walks the whole project; it only needs to be started on one program.
      await runHeadless(ghidra, [
        borlandProjectDir, "Borland/CM.LIB", "-process", "ATEXIT", "-noanalysis", "-readOnly",
        "-scriptPath", scriptPath, "-postScript", "WcFidBuild.java", fidb,
      ]);
      break;
    }
    case "decompile": {
      doFlatten();
      mkdirSync(projectDir, { recursive: true });
      await runHeadless(findGhidra(), [
        projectDir, projectName,
        "-import", flatExe, "-overwrite", "-loader", "MzLoader",
        "-scriptPath", scriptPath,
        "-preScript", "WcSetup.java", manifestPath,
        "-postScript", "WcSwitches.java", manifestPath,
        "-postScript", "WcFid.java", fidb,
        "-postScript", "WcRuntime.java", manifestPath,
        "-postScript", "WcDecompile.java", manifestPath, cDir,
        "-analysisTimeoutPerFile", "7200",
      ]);
      break;
    }
    case "redecompile":
      await runHeadless(findGhidra(), [
        projectDir, projectName,
        "-process", path.basename(flatExe), "-noanalysis",
        "-scriptPath", scriptPath,
        "-postScript", "WcDecompile.java", manifestPath, cDir,
      ]);
      break;
    case "recomp":
      await doRecomp();
      break;
    case "build":
      await doBuild();
      break;
    case "web":
      await cmakeBuild(true);
      break;
    case "discover":
    case "play": {
      // play: in the window, without a time limit, at the normal speed; the game starts again after each recompile
      const play = command === "play";
      mkdirSync(capturesDir, { recursive: true });
      for (let round = 1; round <= 50; round++) {
        const env = play ? { ...process.env, WC_LOADED_DIR: capturesDir }
          : { ...process.env, WC_LOADED_DIR: capturesDir, WC_HEADLESS: "1", WC_CYCLES: process.env.WC_CYCLES ?? "max" };
        const res = spawnSync(wcExe, [gameDir, ...positionals.slice(1)], {
          encoding: "utf8", timeout: play ? undefined : Number(values.seconds) * 1000, env,
          stdio: ["inherit", play ? "inherit" : "pipe", "pipe"],
        });
        if (play) process.stderr.write(res.stderr ?? "");
        const loaded = /wc: call into code loaded at runtime at ([0-9a-f]{4}):([0-9a-f]{4})/.exec(res.stderr ?? "");
        const capture = /wc: saved loaded code to (.+)/.exec(res.stderr ?? "")?.[1];
        if (res.status === 5 && loaded && capture) {
          const off = parseInt(loaded[2]!, 16);
          const image = readFileSync(capture);
          const drivers = readDrivers();
          const same = drivers.find((d) =>
            readFileSync(path.join(loadedDir, d.file)).subarray(off, off + 64).equals(image.subarray(off, off + 64)));
          if (same) {
            console.log(`round ${round}: code at ${loaded[1]}:${loaded[2]} looks like driver ${same.name}, but it ` +
              "wasn't recognised (modified in memory?):");
            console.log((res.stderr ?? "").split("\n").slice(-8).join("\n"));
            break;
          }
          const hash = driverHash(image);
          const ids = readDriverIds();
          let name = ids.find((i) => i.hash === hash)?.name;
          if (!name) {
            // above the program's code (image at 1000:0000): WC2.EXE's reaches past 6000:0000
            const m = JSON.parse(readFileSync(manifestPath, "utf8")) as Manifest;
            const end = 0x1000 + Math.max(...m.segments.map((s) => s.seg + Math.ceil(s.size / 16)));
            let seg = Math.max(0x6000, Math.ceil(end / 0x1000) * 0x1000);
            while (ids.some((i) => i.name === `d${segHex(seg)}`)) seg += 0x1000;
            if (seg > 0xe000) throw new Error("too many drivers");
            name = `d${segHex(seg)}`;
            appendFileSync(driverIdsFile, `${name} ${hash}\n`);
          }
          const seg = parseInt(name.slice(1), 16);
          const d: Driver = { name, seg, file: `${name}.bin` };
          writeFileSync(path.join(loadedDir, d.file), image);
          writeFileSync(driversFile, JSON.stringify([...drivers, d], null, 2));
          appendFileSync(entriesFile, `${(seg * 16 + off).toString(16)}\n`);
          console.log(`round ${round}: new code loaded at runtime (${loaded[1]}:${loaded[2]}), recompiling it as ${d.name}`);
          await importDriver(d);
          await doRecomp();
          continue;
        }
        const missing = /wc: missing code at ([0-9a-f]+)/.exec(res.stderr ?? "")?.[1];
        if (res.status !== 4 || !missing) {
          if (play) break;
          const timedOut = (res.error as NodeJS.ErrnoException | undefined)?.code === "ETIMEDOUT";
          console.log(`round ${round}: ` + (timedOut ? `wc was still running after ${values.seconds} s, no missing code so far`
            : `wc stopped (exit ${res.status ?? res.signal}):`));
          console.log((res.stderr ?? "").split("\n").slice(-8).join("\n"));
          break;
        }
        const known = existsSync(entriesFile) ? readFileSync(entriesFile, "utf8").split(/\s+/) : [];
        if (known.includes(missing)) {
          console.log(`round ${round}: missing code at ${missing} again; the generator could not recompile it:`);
          console.log((res.stderr ?? "").split("\n").slice(-8).join("\n"));
          break;
        }
        console.log(`round ${round}: missing code at ${missing}, adding it as an entry point`);
        appendFileSync(entriesFile, `${missing}\n`);
        await doRecomp();
      }
      break;
    }
    default:
      console.error("usage: node src/cli.ts <flatten|fid|decompile|redecompile|recomp|build|web|discover|play> [--game wc|sm2|wc2] [--exe file] [--borland dir] [--out dir]");
      process.exit(2);
  }
}

function exec(cmd: string, args: string[]): Promise<void> {
  return new Promise((resolve, reject) => {
    const child = spawn(cmd, args, { stdio: ["ignore", "inherit", "inherit"] });
    child.on("error", reject);
    child.on("exit", (code) => (code === 0 ? resolve() : reject(new Error(`${cmd} ${args.join(" ")} failed (${code})`))));
  });
}

main().catch((err: unknown) => {
  console.error(err instanceof Error ? err.message : err);
  process.exit(1);
});
