# wc-decomp

Decompiles Wing Commander's `WC.EXE` to C with Ghidra, driven from TypeScript.

## Building it yourself

This repository contains no game files and nothing derived from them: you need your own copy of
Wing Commander, the GOG release (with the Secret Missions). Everything is worked out on its
`WC.EXE`, SHA-256 `c37d085909175abc95b4a51ffc884913c7d4ea52ba66ca932619ce1f67fa91ca`; other
releases are refused. The native build runs on macOS.

Prerequisites (Homebrew):

```bash
brew install node ghidra cmake ninja sdl3   # Node 23.6 or later; Ghidra 12.2 brings its JDK
brew install emscripten                     # only for the web page (npm run web)
git submodule update --init                 # Nuked-OPL3, for AdLib sound
npm ci
```

Then, with the game folder (the one with `WC.EXE` and `GAMEDAT/`) at `../game`, or passing
`-- --exe path/to/WC.EXE` to every step:

```bash
npm run decompile    # flatten the overlays, analyze with Ghidra (a few minutes)
npm run recomp       # generate C from it and build out/bin/wc
npm run discover     # run it: captures and recompiles the drivers the game loads (a few minutes)
out/bin/wc ../game   # play
```

Everything generated goes to `out/` (ignored by git). `discover` runs the game headless several
times; the drivers it captures depend on the game's sound setting (`WINGCMDR.CFG`, written by the
game's installer; AdLib music is what's tested). The Borland runtime libraries are optional (see
"Naming the Borland runtime" below); they only give the decompiled C nicer names.

`WC.EXE` is a 16-bit DOS program built with Borland Turbo C++ (1990) that keeps more than half
its code in Borland VROOM overlays (an `FBOV` block after the MZ image). Ghidra's MZ loader
ignores those overlays, so the pipeline first **flattens** the EXE:

1. `src/vroom.ts` parses the FBOV header, Borland's segment table and the 40 overlay stubs.
2. `src/flatten.ts` appends each overlay's code to the image as a new segment, adds its
   relocations to the MZ table and rewrites every `int 3Fh` stub entry into a `jmp far` to the
   real function. The result, `out/WC_FLAT.EXE`, is for analysis only and won't run under DOS.
3. `ghidra_scripts/WcSetup.java` (pre-analysis) sets `DS` to DGROUP in all code and names the
   overlay entry points `ovlNN_xxxx`, with a comment giving the original stub address. It also
   turns off Ghidra's "Decompiler Switch Analysis" (see step 5).
4. Ghidra auto-analysis runs.
5. `ghidra_scripts/WcSwitches.java` recovers `switch` jump tables (`jmp cs:[bx+table]`).
   Ghidra's 16-bit x86 model takes CS to be the instruction address rounded down to 64 KB, so
   its own switch analysis reads these tables from the wrong place and creates bogus code and
   functions. The script reads each table from the real segment, disassembles the cases, merges
   back function fragments that analysis split off (loop and case bodies reached by `JMP`s that
   Ghidra turned into fake tail calls), and stores a jump-table override for the decompiler.
   It also turns off Ghidra's "Shared Return Calls" analyzer, which would otherwise split those
   functions again during headless post-analysis. The tables are saved to `out/switches.json`.
6. `ghidra_scripts/WcFid.java` applies the Borland Function ID database (see below), if it
   exists, and turns link names into C names (`_printf` → `printf`).
7. `ghidra_scripts/WcRuntime.java` names the Borland runtime functions that Function ID didn't
   match (or all of them, without the database) by rules:
   `main`/`exit` and `_argc`/`_argv`/`_environ` from the startup code, `atexit`, `abort`,
   `__IOerror`/`errno`, thin DOS wrappers by their `INT 21h` function (`_open`, `_read`, `lseek`, …),
   and compiler helpers (`SCOPY@`, `LXMUL@`, `LDIV@`, …) by a hash of their code bytes. It also marks
   `exit`, `_exit` and `abort` as non-returning and removes the bogus code Ghidra decoded after
   calls to them. Each name gets a plate comment saying which rule produced it; functions you
   have already renamed are left alone.
8. `ghidra_scripts/WcDecompile.java` decompiles every function into `out/c/`, one file per
   original segment (`code_XXXX.c`, `ovlNN.c`), plus `functions.tsv` and `ghidra_types.h`.
   For the recovered switches, the decompiler only knows the case targets, so it prints
   `switch(*(table + i*2)) { case 0x3505b: …`. Using `switches.json`, the script prints them as
   `switch(i) { case 0: case 1: …` instead, folding Borland's `i - N` bias into the case values.

## Usage

Ghidra is found via `GHIDRA_INSTALL_DIR` or `ghidraRun` on `PATH`.

```bash
npm run decompile            # flatten + import + analyze + export C (takes a few minutes)
npm run flatten              # only build out/WC_FLAT.EXE and out/manifest.json
npm run redecompile          # re-export C from the existing project
```

Pass `-- --exe path/to/WC.EXE --out dir` to change the input or output location.

### Naming the Borland runtime with Function ID

`WC.EXE` was built with Turbo C++ 1.0 in the medium memory model. To have Ghidra recognise its
runtime library by code, put these files from a Turbo C++ 1.0 install in one folder (by default
`../tc/extracted`; they're in `MLIB.ZIP` and `XLIB.ZIP` on the install disks) and run, once:

```bash
npm run fid -- --borland path/to/folder
```

| File | Contains |
|---|---|
| `CM.LIB` (required) | the medium-model C runtime |
| `C0M.OBJ` | the medium-model startup code |
| `OVERLAY.LIB` | the VROOM overlay manager |

This imports the libraries into `out/borland` and builds `out/borland-tc10.fidb`; later
`npm run decompile` runs apply it. The database is only registered with Ghidra while a script uses it.

The Ghidra project is saved in `out/ghidra/WC.gpr`. Open it in the Ghidra GUI to rename
functions, define structs and fix types, then run `npm run redecompile` to regenerate the C
with your names.

## A native macOS executable (static recompilation)

`npm run recomp` translates the analyzed program into C that compiles: `ghidra_scripts/WcRecomp.java`
turns every instruction's p-code into C working on an emulated 8086 state (`runtime/wc_rt.h`), one
C function per Ghidra function, and CMake (`CMakeLists.txt`, build tree in `out/build/`) compiles it,
together with the runtime in `runtime/`, into `out/bin/wc` (`brew install cmake ninja sdl3`).

```bash
npm run recomp               # generate out/recomp/*.c and build out/bin/wc
npm run build                # only rebuild (cmake --build out/build)
npm run discover             # run it; add code reached only through pointers in data, rebuild, repeat
out/bin/wc ../game           # run (WC_TRACE=1 traces DOS calls, WC_TRACE=2 every instruction)
```

The runtime (`runtime/`) loads and relocates the program image like DOS would (`WC_LOAD_SEG`,
default `0200`) and provides a small DOS on top of the game folder: files, current directory,
memory, vectors, date and time. Hardware interrupts are emulated (8259 PIC,
8253 timer at the rate the game programs, keyboard from the terminal or `WC_KEYS`), every vector
starts at a native BIOS entry (F000:nn), and `INT n` goes through the vector table, so the game's
own handlers run. BIOS video, keyboard and mouse services are stubs. Generator
details that matter:

- Ghidra's 16-bit model computes CS as the address rounded down to 64 KB and drops some segment
  override prefixes; the generator substitutes the real segment and the prefix's register.
- Values from MZ relocations become `value + wc_reloc`, so the image can load at any segment.
- Calls keep a shadow return stack, so a jump to the expected return address (Borland's
  `_setargv` returns that way) is a return.
- Every overlay stub entry, plus the addresses in `entries.txt`, is decoded with Ghidra's
  pseudo-disassembler, since such code is often missing or misaligned in the listing.
- The VROOM overlay manager still initialises as on DOS (it opens WC.EXE and reserves its
  buffer, though the overlays are resident); the game's INT 21h hook depends on its state.

### Code loaded at runtime (drivers)

The game unpacks its Origin FX drivers (`TM.DRV`, `SUPERTM.DRV`, ... are compressed containers) into
the heap and calls them through far pointers. `npm run discover` handles that too: when the
program calls into memory that isn't program code, the runtime saves the 64 KB segment
(`out/loaded/`), discover imports it into the Ghidra project at its own 64 KB-aligned virtual
segment (6000, 7000, ..., by a hash of the driver in `drivers.txt`, since the game loads them in no
fixed order; `WcDriverEntries.java` seeds the entry points and only reachable code is
decoded), and `WcRecomp.java driver` recompiles it as position-independent code. At runtime a
driver is recognised by its instruction bytes wherever it is loaded, and writes into those bytes
(self-modifying code) are reported; none have been seen.

Overlay code reads its own constants (switch tables) through CS from a pristine copy of the
image, since the heap overlaps the flattened overlays' addresses at runtime.

### The window

`out/bin/wc` opens a window (SDL3) showing the mode 13h picture at
4:3, and passes the keyboard to the game as PC scancodes (the arrow and other grey keys as their
keypad twins) and the mouse to a mouse driver (INT 33h, `runtime/mouse.c`; the game draws its own
pointer, so the Mac's is hidden over the window). In full screen the pointer is captured, so the
menu bar and the Dock don't pop up at the screen edges; the game gets the mouse's movement, twice
as fast as the picture is wide; Cmd+Left/Right change that by 25% (`WC_MOUSE_SPEED` sets it; the title
shows it, and in full screen the terminal). Cmd+F toggles full screen, Cmd+Q quits. The recompiled program runs on a thread of
its own; the window shows the frame buffer and palette as they were at the last vertical retrace,
which runs at the VGA's 70 Hz in real time (port 3DAh). `runtime/host.h` is the whole interface
between the two, so the emulation itself never includes SDL.

Wing Commander paces its animation and space flight by CPU speed, so the runtime limits it to
8334 instructions per millisecond. Cmd+Up/Down in the window change that by 20% (the title shows
the value); `WC_CYCLES=n` sets it at start (instructions per ms, like DOSBox's `cycles`), and
`WC_CYCLES=max` runs as fast as possible. In space flight the game draws more per frame when
enemies are close, so there the runtime paces frames instead: 15 per second (`WC_FPS`), running
at full speed in between; in flight Cmd+Up/Down change the frame rate instead, by 20% too, and
leave the instruction rate alone. `WC_STATS=1` prints the frame rate.

`WC_HEADLESS=1` runs without a window (`npm run discover` does), taking keys from the terminal or
`WC_KEYS`. `WC_SCREENSHOT=file` saves the VGA palette and frame buffer when the program stops (also
on Ctrl-C or a timeout); `tools/screenshot.py file out.png` makes a PNG.

Sound: AdLib music. The OPL2 at ports 388h/389h is emulated by
[Nuked-OPL3](https://github.com/nukeykt/Nuked-OPL3) (LGPL 2.1, a git submodule in
`third_party/`: after cloning, `git submodule update --init`); `runtime/opl.c` makes the samples in
real time and the window plays them through SDL3. `WC_WAV=file.wav` also records them (works
headless too).

Not supported yet: text-mode output, Roland MT-32 and Sound Blaster digital sound, joystick.

### Starting at any mission

`npm run mission` writes a save game that starts at a chosen mission into a free slot of the
game's `GAMEDAT/SAVEGAME.WLD` (copying the file to `SAVEGAME.BAK` first). In the game, choose
"Continue Campaign", click the sleeping pilot of that slot in the barracks, then leave through the
right-hand door for the briefing.

```bash
npm run mission -- --list                 # the mission trees of the Vega campaign and Secret Missions
npm run mission -- gimle.2                # series by its system's name (or number), mission from 1
npm run mission -- sm1:midgard.1 --name Blair --callsign Maverick
npm run mission -- --slot 3 venice.4      # needed when no slot is free: overwrites slot 3
```

The save holds a new pilot's state (taken from `WC.EXE`, as the game copies it when a campaign
starts) with a series history along a path through the tree, preferring won series; scores, kills,
medals and rank are those of a new pilot. `--game dir` picks another game folder, `--title` the
name shown in the barracks. Secret Missions 2 isn't run by `WC.EXE` (it's `SM2.EXE`), so it can't
be chosen.

## In the browser (WebAssembly)

The same runtime builds with [Emscripten](https://emscripten.org) (`brew install emscripten`) into a
web page: `runtime/host_web.c` takes the place of the SDL window, and `web/index.html` shows the
picture on a canvas (full screen with ⌘/Ctrl+F or the button; the pointer is captured there), passes
the keyboard and mouse to the game and plays the AdLib music with an AudioWorklet.

```bash
npm run recomp && npm run discover   # as for the native build, if not done yet
npm run web                          # out/web: index.html, wc.js, wc.wasm, sw.js
npm run serve                        # http://localhost:8080
```

On the page, drop your game folder (or choose it, or its files): the files are kept in the browser's
IndexedDB, so this is needed once. Whatever the game writes (save games, high scores, in
`SAVEGAME.WLD`) goes to IndexedDB as well and is there again after a reload, also when you give the
page the game files anew; "Download save games" makes a copy. The page asks the browser to keep its
storage persistent, but a browser may still clear it, so keep copies of save games you care about.
Logs go to the browser console; URL parameters set the `WC_*` variables (`?WC_STATS=1`).

The emulation runs on a thread of its own, which needs `SharedArrayBuffer`, so the page has to be
cross-origin isolated: `npm run serve` sends the headers for that, and on a static host that can't,
`sw.js` adds them (the page reloads once). The built page contains the recompiled game, so like
`out/bin/wc` it's for your own use, not for publishing.

## Expect pseudocode, not compilable C

The output reads like C but won't compile: it has no original names or types, it uses
segment arithmetic and Ghidra helpers such as `CONCAT22`, and it touches DOS and hardware
directly (`int 21h`, VGA memory, ports). Treat it as the starting point for a rewrite.

## License

BSD 2-Clause (`LICENSE`), for this repository's code. Nuked-OPL3 (`third_party/`, linked into
`out/bin/wc`) is LGPL 2.1. Wing Commander itself is not included and remains its owners' property.
