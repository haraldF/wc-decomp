# CLAUDE.md

Notes for working on this project: what was found out the hard way. The README describes the
pipeline and usage; this file records the facts and pitfalls behind it.

## Goal

A native macOS (later WebAssembly) executable of Wing Commander 1 (`../game/WC.EXE`), made by
statically recompiling the DOS program to C. Readable decompilation (`out/c/`) is a side product;
the user doesn't care about naming or understanding the code, only about running it.

Never commit game files or anything derived from their bytes: `WC.EXE`, `GAMEDAT/`, `*.DRV`,
captured driver images (`out/loaded/*.bin`), `image.bin`/`relocs.bin`, the Borland libraries
(`../tc/`), screenshots. Everything under `out/` is generated and ignored.

## The executable

- Borland Turbo C++ 1.0 (1990), **medium memory model** (far code, near data, SS = DS).
  Runtime: `CM.LIB`, startup `C0M.OBJ`, overlay manager `OVERLAY.LIB` (from `MLIB.ZIP`/`XLIB.ZIP`
  in `../tc/`). DGROUP is segment 0x1231 (relative to load segment 0x1000).
- Borland VROOM overlays (`FBOV` block after the MZ image): 40 overlays, 867 stub entries.
  - Stub header is 0x20 bytes; entries are `CD 3F off16 00` (`int 3Fh`).
  - Overlay relocation words are **segment-table offsets** (index × 8), not segments; map them
    through the segment table (flags: 0 data, 1 code, 3 stub, 4 other).
- Flattening appends overlays as segments and rewrites stub entries to `EA off seg` (jmp far).
- The VROOM manager must still initialise as on DOS: the game's INT 21h hook uses VROOM's file
  handle (at 2058:00b8). Disabling the init was tried and reverted.
- Graphics drivers `TM.DRV`/`SUPERTM.DRV` (Origin FX) are compressed; the game unpacks them into
  the heap and calls them through far-pointer tables at `DS:7a4a..7aa0`. No self-modifying code.
- The game sets mode 13h (320×200, 256 colours) and programs the DAC via 3C7/3C8/3C9.
- `WINGCMDR.CFG`: the user's real config is `v a904 z`: AdLib music. Its driver is captured as
  `d8000`; it writes OPL2 registers at 388h/389h (~150/s) without probing for the chip. `v z` (no
  sound) loads no sound driver. Always test discovery with the user's config (`../game`), since
  each config loads different drivers. The game polls the joystick at port 201h constantly.
- Each driver starts with a zero word and a table of entry offsets, ended by a value < 200h
  (0022h graphics, 0100h music); `runDriver` seeds the whole table (d6000: 22, d7000: 5, d8000: 9),
  so driver functions no longer turn up one stop at a time (the briefing stopped at 6401d).
- Drivers also dispatch through unbounded jump tables (`jmp cs:[di+04ac]` in d7000, 64 entries,
  ended by zeros); `switchTargets()` reads those in drivers while the words look like code offsets
  (200h..BFFFh), and it now runs over Ghidra's functions too, not only fragments (71884).
- Drivers also call through code pointers they store themselves: `mov word cs:[06c2],1c48` then
  `call cs:[06c2]` (d7000: 32 such callbacks), also via a register: `mov ax,65d2` within 4
  instructions before `mov cs:[06c4],ax`. `callbackTargets()` finds them in the decoded code
  only, repeating until no new ones appear: the 64 KB snapshot also holds whatever followed the
  driver in memory (d6000's snapshot ends in d7000, loaded right after it), so scanning raw bytes decodes junk.
- And through small tables right behind the routine: `mov di,cs:[04aa]; add di,2475; call cs:[di]`
  (8 sites in d7000, 2 entries each). `registerTableTargets()` reads from the add/mov constant up
  to the next decoded instruction (72d41).
- Captured driver images and `out/loaded/drivers.json` are in ignored `out/` (game bytes); after
  wiping `out/`, `npm run discover` captures them again.
- `WC_KEYS` counts timer interrupts, and loading takes longer with music: Esc before ~tick 1000
  lands while the game is still loading and is ignored. `WC_KEYS=1000:esc,1100:esc` reaches the
  main menu with music on. Flight simulator, name entry and the rec room:
  `...,1400:+space,1406:-space,1600:b,1610:l,1620:a,1630:i,1640:r,1660:enter,1700:m,1710:a,
  1720:v,1740:enter,2000:+space,2006:-space` (after the campaign-start keys).
- The menus sample keys, so a scripted tap (make and break at once) is often missed: hold keys for a few
  ticks (`+up`/`-up`). Held keys repeat like a PC keyboard (after 0.5 s, ~11/s); the pointer in
  the rooms moves only on repeats, e.g. `2300:+right,2500:-right` reaches "Enter barracks". Starting the campaign: `WC_KEYS=1000:esc,1100:esc,1300:+up,1306:-up,
  1320:+up,1326:-up,1340:+up,1346:-up,1360:+up,1366:-up,1400:+space,1406:-space`.
- A "quit" in the window is usually exit 4 (missing code) or 5 (new driver): the reason is on
  the terminal. Paths reached rarely need several runs; replay them and add each address.
- Port 201h (joystick) must read FFh when there is none: 00 means a stick in the corner with all
  buttons pressed, which kept the menu cursor stuck.

## Ghidra (12.2, Homebrew; JAVA_HOME from the `ghidraRun` wrapper, openjdk@25)

x86 real-mode modelling bugs the scripts work around:

- CS is computed as `(inst_next >> 4) & 0xf000` (address rounded down to 64 KB) instead of the
  real segment. Consequences: switch tables are read from the wrong place (hence `WcSwitches.java`
  and "Decompiler Switch Analysis" disabled), and the generator substitutes the real segment
  (`realSegment()`, preferring Ghidra's segment when it is a manifest segment start, because
  segments overlap, e.g. at 1000:3be4).
- Relative near CALL/JMP/Jcc targets are wrapped within that fake CS too: `push cs; call near`
  at 2fd2:0554 back to 2fd2:0047 became 3000:fd67, a function in another overlay. It corrupted BP,
  then SP (SP=00a2 with SS=DS), so Borland's near malloc saw no room below the stack: "ERROR: Out
  of memory for SPACE BUFFER". `nearTarget()` uses next + signed displacement instead (67 fixes)
  and seeds the corrected targets. Symptom to remember: low SP/BP in DGROUP, entries that make no
  sense. Careful: 3fd28/3fd1f were reached wrongly through this bug, but are real code too
  (removing them made the briefing stop). Before removing an entry, check it isn't legitimate.
- Ghidra functions sometimes end right before code they jump or fall into (e.g. 3fd0b before
  3fd1f); the generator seeds every function exit target that is in no function.
- Code Ghidra put 64 KB off is only decoded as fragments, so `WcSwitches` never sees its switch
  tables; `switchTargets()` in the fragment decoder reads Borland's `cmp bx,N; ja; shl bx,1;
  jmp cs:[bx+table]` itself (the hangar stopped at 3ff75, then 3ff40: two cases of one switch).
  Also Borland's sparse switch (`mov cx,N; mov bx,table; mov ax,cs:[bx]; cmp; jz; inc bx; inc bx;
  loop; …; jmp cs:[bx+2N]`: N values, then N targets), `sparseSwitchTargets()` (4d7ef; 6 found).
- Segment-override prefixes are sometimes dropped; the generator re-applies them.
- `JMPF m16:16` (jmp far through memory) is modelled as a near jump.
- `IRET` pops into the `flags` word (register 0x280) only, without unpacking CF/IF/...; the
  generator calls `set_flags_word()`.
- "Shared Return Calls" and fragment splitting break functions; `WcSwitches` turns it off and
  merges fragments back.
- Headless: a second `-scriptPath` overrides the first (join with `;`); MzLoader always loads at
  segment 0x1000; driver images use BinaryLoader with `-loader-baseAddr seg:0000`.
- `JumpTable` constructor takes 4 arguments: `new JumpTable(at, targets, true, 0).writeOverride(f)`.
- Function ID: link names are converted to C names (strip one leading `_` unless all uppercase);
  the fidb is registered only while a script runs.

## Static recompilation (WcRecomp.java + runtime/)

- p-code → C on an emulated 8086: `R[]` is Ghidra's register space (offsets in `wc_rt.h`), `M[]`
  1 MB of memory. One C function per Ghidra function with an entry `switch` and gotos.
- `run(linear)` is for runtime addresses (indirect jumps/calls), `run_code(code)` for compile-time
  addresses of the 1000:0000 image. Mixing them up sends static targets through relocation.
- Relocatable: constants from MZ relocations become `((value + wc_reloc) & 0xffff)`; the image
  loads at `WC_LOAD_SEG` (default 0x0200).
- Shadow return stack (`push_ret`/`pop_ret`/`is_return`): Borland's `_setargv` "returns" by
  jumping to a saved address. A bug here once looked like an out-of-memory failure in VROOM.
- Heap memory overlaps the flattened overlays' addresses at runtime, so
  - `is_program()` decides by segment (overlay code always runs with CS at its segment),
  - overlay CS-relative reads go through `mrd_code()` to a pristine relocated image copy.
- Missing code: runtime exits 4 (`wc: missing code at X`); `npm run discover` adds X to
  `entries.txt` and recompiles. Code loaded at runtime: exit 5, the 64 KB segment is captured
  and recompiled as a driver at virtual segment 6000, 7000, ... (`WcDriverEntries.java` seeds only
  observed entries; "Function Start Search" off so heap bytes aren't decoded). Drivers are
  recognised at runtime by their instruction bytes; writes into them are reported.
- Discovery is timing-dependent; several rounds may be needed. Bugs elsewhere produce bogus
  entries (garbage addresses). `entries.txt` was cleaned by hand (removed: 13dfb, 3ec5e,
  3f5f6, 0007e, 4736f, 4aba0, 56af9, 4e2fe, 4bd1c, 4be19, 468c6, 60000); 82 entries remain.
  It lives in the repository root (not `out/`), so commit it after discover rounds.
- Drivers get their virtual segment from `drivers.txt` (repository root), by the SHA-256 of their
  first 1 KB: the game doesn't load them in a fixed order (a fresh clone captured the AdLib driver
  first and named it d6000, so entries.txt's driver entries landed in the wrong drivers and the
  runtime reported writes into "driver code"). A new driver gets the next free segment there.
- No game or library bytes in the repository, not even signatures: WcRuntime matches Borland
  helpers by length + SHA-256 prefix, the runtime finds the frame copy by its driver address,
  and `flatten` refuses any WC.EXE but the GOG one (by SHA-256).
- Build: `CMakeLists.txt` (Ninja, `out/build/`, `compile_commands.json` there); `cli.ts` configures
  it once and runs `cmake --build`. Generated code is compiled with `-w -O1`; runtime with `-Wall`.
- `writeIfChanged` keeps unchanged generated files so builds stay incremental. Each `seg_*.c`
  declares the functions it uses itself: `wc_gen.h` gains entries every discover round, and with
  exact header tracking every file including it would recompile (a full build is ~30 s). Only
  `wc_dispatch.c` includes it. WcRecomp deletes `seg_*.c` files it no longer writes, since the
  build globs them (a stale `seg_unknown.c` was being linked in).

## PC emulation (runtime/pc.c, dos.c)

- 8259 PIC (mask port 21h, EOI port 20h, initial mask 0xb8), 8253 PIT (IRQ0 in real time at the
  programmed rate; counter readable with latch, lo/hi byte, mode 3 counts by 2), keyboard
  ports 60h/61h/64h from the TTY or `WC_KEYS`.
- Every IVT entry starts as a native BIOS entry F000:n; `INT n` goes through the IVT so the game's
  own handlers run. Hardware IRQ vectors (8, 9, 1Ch) end with a full IRET; services return like
  `RETF 2` (keep CF; restore IF/TF/DF). Restoring the caller's flags lost DOS's CF once.
- BIOS keyboard: INT 9 (when the game chains to it) turns scancodes into keys in the BDA buffer
  (0040:001E..003D, shift flags at 0040:0017); INT 16h and INT 21h 01/06/07/08/0B/0C read it.
  Waiting services call `pc_wait()`, which lets timer and keyboard interrupts run. The simulator's
  high-score name entry uses INT 21h AH=07.
- Mouse (runtime/mouse.c): the game resets it (INT 33h AX=0), then polls AX=3 constantly and
  sets the position with AX=4 (e.g. to 320,100); positions are in the driver's 640x200 space in
  mode 13h. Window events arrive over a lock-free ring; a click is reported until the program has
  read the buttons once, and one shorter than 60 ms is reported as held for 60 ms (the barracks
  missed instant clicks even so: WC_KEYS `lmb` taps work now). Headless: WC_MOUSE=1 and WC_KEYS entries
  `@x.y` (move, 320x200), `lmb`/`rmb` (+/- to hold/release).
  In flight the game only polls AX=3 (absolute position) and steers by the pointer's distance from
  the centre, like a joystick, without re-centring it. Captured movement (full screen) at 1:1 (a
  movement as wide as the picture crosses it) felt sluggish and imprecise (with macOS's trackpad
  acceleration). A curve against the acceleration (d -> 4·(d/4)^0.5) made it worse: everything got
  slower. Now a plain speed factor, default 2 (felt right), Cmd+Left/Right ±25% (WC_MOUSE_SPEED; the page:
  ⌘/Ctrl+←/→, ?mouse=); the page also asks for `unadjustedMovement` pointer lock. Clicking "Start Vega Campaign":
  `WC_KEYS=1000:esc,1100:esc,1300:@160.55,1350:+lmb,1356:-lmb` (at WC_CYCLES=max).
- Expanded memory (runtime/ems.c, INT 67h, LIM 4.0, 8 MB, page frame E000h): GOG's DOSBox has
  `ems=true`. Without EMS the game keeps everything in conventional memory, and launching from a
  Secret Missions 1 save the launch animation (SCRAMBLE.VGA, drawn by d6000) was decoded over
  the music that was playing (FUN_19c5_0bda, a MIDI sequencer in the timer interrupt): it read a
  garbage delay (thousands of ticks) and stalled with all notes held, a loud drone in flight.
  The game finds the driver by "EMMXXXX0" at F000:000A (INT 67h's segment), allocates all pages
  ("Expanded Memory fully used" is normal) and maps 4 at a time (~12000 AH=44h in 15 s); mapping
  copies pages in and out of the frame. WC_EMS=0 turns it off.
- BDA 0x463 must be 0x3D4 (CRTC base) or the retrace wait polls port 6. Port 3DA toggles.
- DOS INT 21h: the functions the game uses (see `dos.c`); file names are case-insensitive against
  the game folder.

## Window (runtime/host_sdl.c, vga.c)

- SDL3 on the main thread (macOS needs it), the recompiled program on a pthread with a 256 MB
  stack. `host.h` is the only interface: `host_open`/`host_run` one way, `vga_frame`/`pc_key`
  the other. Keys go through a lock-free ring; frames are copied under a mutex at each emulated
  vertical retrace (`vga_poll`, called from `irq_poll`).
- Port 3DAh follows a real 70.09 Hz VGA timing (blanking bit 0, retrace bit 3), so the game is
  paced like on hardware. It used to just toggle.
- With the CPU speed limit the program runs in ~1 ms bursts and missed most 64 us retraces, so
  fades (one palette step per frame) crawled while CPU-paced animation didn't. A retrace that began
  since the last read of 3DAh is now reported once.
- The game sets its palette with INT 10h AX=1012h (DAC block), not only through ports 3C8h/3C9h:
  with INT 10h ignored, every picture looked nearly black. `vga_bios` handles mode set/get, the
  DAC calls, and VGA detection (AH=12h BL=10h, AH=1Ah).
- Screen recording isn't permitted for the agent here; to check the window's output, temporarily
  read it back with `SDL_RenderReadPixels` + `SDL_SaveBMP` before `SDL_RenderPresent`.

## Sound (runtime/opl.c)

- AdLib: OPL2 at 388h (index; reading it gives the timer status) and 389h (data), emulated by
  Nuked-OPL3 (third_party/, submodule, LGPL 2.1) at its native 49716 Hz. Samples are made on the
  emulation thread in `opl_poll()` up to the current real time (also right before each register
  write), and pushed into an SDL3 audio stream (60 ms prefilled; cleared if over 0.25 s queued).
- `WC_WAV=file` records the sound, also headless: the intro's music starts after ~3 s, peaks
  around +-1800 (quiet, like the chip's raw output).

## CPU speed

- Wing Commander paces animation and space flight by CPU speed. The runtime runs at most
  `WC_CYCLES` instructions per ms, checked every 1024 instructions in `irq_poll` (`throttle()`);
  `WC_CYCLES=max` is unlimited (~400–600 M/s). Cmd+Up/Down in the window change it by 20%; in
  flight (`pc_in_flight()`: frames are being paced) they change the frame rate instead, each
  separately (they used to scale both together).
- GOG's DOSBox uses `cycles=4000`, but at 4000 the user found the game far too slow (our count
  is not DOSBox's cycle count). At 10000 the rest felt right but the simulator a little too fast;
  two Cmd+Down steps (~6950) suited it, so the default was 7000. Since flight is paced by frames
  (below), the intro decides: 10000 was a little too fast, one Cmd+Down step from there (8334,
  with 15 fps in flight) felt right, so those are the defaults.
- In flight, frames are paced instead (pc_frame, `WC_FPS`, default 15; 25 and 21 felt too fast, "hectic", 18 was fine, 15 better in combat): the frame copy is entry 4
  (offset 7454) of the second graphics driver, d7000, recognised in `run()` by that address. Between
  two frame copies the game may run 3 frames' worth of instructions (3 x cycles x 1000/fps) at full
  speed; beyond that the instruction limit applies again. (A 0.5 s full-speed window after the last
  frame copy made the autopilot sequence, which doesn't use that routine, flash by.) At a fixed 7000 cycles flight ranged
  13-42 fps (frames up to 119 ms, enemies close = more drawing); paced it holds 25-26 fps, longest
  ~45 ms. The game advances per frame (unpaced, the scripted pilot dies within a second).
  `WC_STATS=1` prints frames/s. Flight never waits for the retrace (no 3DAh reads).
- Quick start (pc.c, `WC_WARP=0` off): until the first visible frame (vga_poll) or the first key or
  mouse button from the host, emulated time runs 20x (pc_now_ns adds 19x the real time spent
  warping, so the clock stays continuous) and instructions aren't limited; opl_poll makes no
  samples meanwhile. Before the intro the game mostly waits: after "... music will play." it shows
  the text until a timer runs out or a key is pressed (ovl28_0151). The first picture came after
  5.2 s at 8334 cycles (3.3 s even at max: the waits are timed); now ~0.6 s. WC_KEYS ticks are
  emulated timer interrupts, so the scripts below are unaffected.
- At limited speed, key timings in menus are unreliable; click with the scripted mouse:
  `WC_MOUSE=1 WC_KEYS=1000:esc,1100:esc,1300:@160.55,1350:+lmb,1356:-lmb,1500:@160.55,1550:+lmb,1556:-lmb`
  reaches the simulator at the default speed.
- The `WC_KEYS` sequences in this file were made at full speed: use them with `WC_CYCLES=max`
  (`npm run discover` sets it unless WC_CYCLES is given).

## State

The intro runs natively (~400–600 M instructions/s); a frame-buffer screenshot shows the credits.
The SDL3 window shows it live, takes the keyboard and mouse, and plays AdLib music. Missing:
MT-32 / Sound Blaster digital sound, text mode, joystick.

The Secret Missions 2 (`SM2.EXE`) run too, built the same way with `--game sm2` (see below).
The same runs in the browser (`npm run web`, see below). Sound via Nuked-OPL3 (LGPL) on 388h/389h, later libmt32emu — not DOSBox Staging's audio code (GPL,
tied to its mixer).

## Web (runtime/host_web.c, web/index.html)

- Same emulation, Emscripten build of the same CMakeLists (`npm run web`: emcmake, `out/build-web`,
  output `out/web`: index.html, sw.js, wc.js, wc.wasm ~28 MB; compiles in seconds). `host_web.c`
  replaces `host_sdl.c`; `main()` starts the game pthread and returns (`emscripten_exit_with_live_runtime`),
  no Asyncify, no PROXY_TO_PTHREAD. The page calls `web_frame()` per animation frame (RGBA) and
  `web_key`/`web_mouse`/`web_cycles`; sound is a ring of 16-bit stereo frames in the shared memory
  that an AudioWorklet reads (`web_audio_info()`: ring, size, written, read, rate) and resamples.
- The game thread's C stack: measured at most ~2 KB natively (intro, menus, campaign, flight); the
  web build gives it 16 MB (the native 256 MB is far more than needed).
- pthreads need SharedArrayBuffer, i.e. a cross-origin isolated page: `npm run serve` sends
  COOP/COEP; elsewhere `sw.js` (a service worker) adds them and the page reloads once.
- Game files: dropped/picked in the page, stored in IndexedDB ("files"), written to MEMFS `/game`
  at start; WC.EXE is checked by SHA-256 in the page. Files the game writes: `dos.c` tracks changed
  handles and calls `host_file_saved()` on close/delete; the page copies them to IndexedDB
  ("saves"), which overrides "files" at the next start. SAVEGAME.WLD also holds the high scores.
- Exiting: the emulation ends through `host_exit()` (DOS 4Ch, errors). In the browser plain exit()
  from the game thread did nothing visible (the runtime is kept alive: no onExit, no atexit, stdout's
  last line unflushed), so `host_exit` flushes (fsync for the partial line), tells the page
  (black screen, out of full screen, "Play again" reloads) and ends the thread with pthread_exit.
- URL parameters `WC_*` become environment variables (`?WC_STATS=1`, `?WC_TRACE=1`); stdout and
  stderr go to console.log. In flight the browser holds 15-16 fps like native.
- Browser automation sends clicks/keys with press and release at once and clicks without a move
  first; the game missed those (also natively with WC_KEYS taps). Fixed for everyone: the mouse
  driver reports a click shorter than 60 ms as 60 ms long, host keys released within 60 ms have
  their break code held back, and the page moves the pointer 40 ms before a press somewhere new.
- The built page contains the recompiled game (and the drivers' bytes, image.bin): don't publish
  `out/web`; it's for your own use like `out/bin/wc`.
- The app's browser pane refuses full screen ("Permissions check failed"); test that in a browser.

## Save games and the campaign (src/mission.ts)

- `SAVEGAME.WLD`: 8 slots of 33Ch bytes: name (17), used flag (+11h), then memory as is: the
  roster DS:9912 (342 bytes: 9 pilots of 38, name 14 + callsign 14 + ..., the player last), the
  campaign state DS:c240 (44h) and the last mission's flight records DS:d256 (400). The game makes
  new slots from stack junk, so only the used flag counts. Load: `ovl18_08a3`; save: `ovl18_06f9`.
- Campaign state: +0 pointer to the player's roster entry (set after loading, `ovl17_0000`),
  +2 war status (from the series transition), +15h mission (from 0), +16h series (from 1),
  +17h series completed and +18h which, +34h/+36h day/year, +42h campaign (0 Vega, 1 SM1).
  A new campaign copies it from DS:17c8 and the roster from DS:180c (`ovl17_10c9`).
- `CAMP.00n` (Origin resource file: total size, then 24-bit offsets with a flags byte, 1 =
  LZW of 9-12 bits, 100h reset, 101h end; widen when the next code reaches 2^bits): entry 1 is the
  series table, 13 x 5Ah bytes: +2 missions, +3 score to win, +5 ending (40h victory, 41h defeat),
  +6/+7 next series and war status when won, +8/+9 when lost, missions at +0Ah (14h each).
  System names: `MODULE.00n` entry 5 (40-byte strings, index = series). Vega's series 12 is Venice
  (victory), 13 Hell's Kitchen (defeat); there is no "Hellespont".
- WC.EXE's campaign file table (DS:1e66) has Vega and SM1 only; SM2 is `SM2.EXE`'s. A save with
  campaign 2 overran the stack while loading.
- The game also has hidden developer arguments: `Origin` (exact case) enables `s<series>`,
  `m<mission>` (sets the start, as in the new-campaign template), `l`, `w<n>`, and `-b`/`-k`;
  arguments are read after the words of `WINGCMDR.CFG`. `-k` clears DS:00BA, without which the
  flight code applies no damage to the player's ship (ovl11): `wc ../game Origin -k`, the page
  `?args=Origin%20-k`. Arguments after the game folder go to the PSP command line; `?` prints the
  version (F3.1). At runtime DS is load segment + 1231h = 1431h: DS:x is linear 14310h + x (WC_WATCH).
- Barracks bunks are the slots (hotspot table DS:1d12): the sleeper loads, the rest of the bunk
  saves; the right-hand wall (300,60) goes to the mission. Clicking a sleeper at full speed:
  `WC_MOUSE=1 WC_KEYS=1000:esc,1100:esc,1300:@160.100,1350:+lmb,1356:-lmb,1700:@137.90,1750:+lmb,
  1756:-lmb,1850:+y,1856:-y,1950:+y,1956:-y,2700:@300.60,2720:@300.61,2750:+lmb,2756:-lmb` (slot 1;
  the dialog's timing varies, hence the repeats) reaches the briefing in ~60 s.

## Secret Missions 2 (SM2.EXE)

- A program of its own (GOG: SHA-256 900f379b...), same engine and compiler, laid out differently:
  DGROUP 11E8h (DS = load seg + 11E8h), SS:SP 1F7Ah:0080h, 40 overlays / 864 entries. Nothing
  recompiled for WC.EXE applies; `--game sm2` runs the whole pipeline on it into `out/sm2/`
  (`out/sm2/bin/sm2`), with `entries-sm2.txt`. `cli.ts` writes `wc_program.c` (exe name, entry,
  stack from the header) for the runtime; the environment's program path is that name (VROOM
  opens it for the overlays).
- It loads the same drivers, but writes other values into their first bytes (the header words
  at 0Ch/0Eh, 2Eh/30h), so the first-1-KB hash differs: `drivers.txt` lists a second hash for
  d6000/d7000 (it first captured them as d9000/da000). Same names keep `FRAME_COPY` (d7000:7454)
  valid.
- Reads `CRUSADE.CFG` (not WINGCMDR.CFG), saves to `CRUSADE.WLD` (same slot layout). Its campaign
  file table (DS:1F00) has camp.000-002; its campaign is 2. New-campaign template at DS:171C
  (state) / DS:1760 (roster). Series names repeat (Firekka, Corsair, Charon): use numbers.
- Pilots go from WC to SM2 with GOG's TRANS2.EXE (text mode, not recompiled; only from Venice or
  SM1 saves, into CRUSADE.WLD). `mission.ts --transfer n` copies the roster of SAVEGAME.WLD's slot
  n into a new campaign's state instead; SM2 loads it (checked: barracks, briefing).
- WC.EXE entries mostly don't carry over by bytes (data offsets in the instructions differ);
  the few that matched seeded entries-sm2.txt, plus all driver entries.
- Copy protection after the intro: "Enter 5:" on a black screen. The question (from INTRO1.DAT,
  ovl38) is drawn into a 1x1 window (rect DS:3BC8 = 1,1,1,1) and the result is skipped by a
  `jmp` over "Protection failed" (56f8:0265): GOG's patch. Any answer + Enter goes on.
- "Start Secret Missions 2" (like WC1's start) runs a short automatic simulator round, whose
  high-score entry is the name entry, then the rec room. Timings (timer ticks ~60/s at max speed):
  `WC_MOUSE=1 WC_KEYS=2000:esc,2100:esc,2400:+1,2404:-1,2450:+enter,2454:-enter,2700:@60.72,
  2750:+lmb,2756:-lmb,9200:b,...,9260:+enter,9266:-enter,9300:m,...,9340:+enter,9346:-enter,
  11000:+space,11006:-space` reaches the rec room; `13000:@305.100` + click the barracks;
  `15500:@300.60` + click the briefing (clicking at 60,72 keeps the pointer off the simulator).
  Continue Campaign is at 60,115; a sleeper at 137,90 asks "(Y/N)" ~700 ticks later. Esc in the
  briefing goes back to the barracks; Space steps through it into flight. WC_KEYS takes up to
  1024 entries. Discovery: `discover --game sm2 --seconds 900` with such keys (and `--exe` in a
  scratch copy of the game folder holding a save from the mission tool) found nothing missing
  through intro, protection, menus, simulator, rec room, barracks, briefing and a mission flown
  (autopilot key, no combat steering) back to the barracks.

## Conventions

- TypeScript run directly by Node (≥ 23.6), ESM, `npm run typecheck`. Java Ghidra scripts in
  `ghidra_scripts/`. C11 runtime in `runtime/`, tab-indented, short comments.
- Debug env vars: `WC_TRACE` (1 DOS/IRQ, 2 every instruction, 3 more), `WC_KEYS`, `WC_LOAD_SEG`,
  `WC_POKE`, `WC_WATCH`, `WC_SCREENSHOT`, `WC_LOADED_DIR`, `WC_HEADLESS`, `WC_CYCLES`, `WC_MOUSE`, `WC_WAV`, `WC_EMS`, `WC_WARP`. Ctrl-C prints a report.
