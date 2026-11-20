# CLAUDE.md

Notes for working on this project: what was found out the hard way. The README describes the
pipeline and usage; this file records the facts and pitfalls behind it.

## Goal

A native macOS (later WebAssembly) executable of Wing Commander 1 (`../game/WC.EXE`), made by
statically recompiling the DOS program to C. Readable decompilation (`out/c/`) is a side product;
the user doesn't care about naming or understanding the code, only about running it.

Never commit game files or anything derived from their bytes: `WC.EXE`, `GAMEDAT/`, `*.DRV`,
captured driver images (`out/loaded/*.bin`), `image.bin`/`relocs.bin`, the Borland libraries
(`../tc/`), screenshots, the Roland ROM images (`roland-mt-32-roms/`, ignored). Everything under `out/` is generated and ignored.

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
- Joystick (runtime/joystick.c): the game measures an axis by counting how many of N reads find
  its bit still 1 after a write (FUN_198a_007c), not by time, so the emulated one-shot lasts a number
  of reads (centre 256, +-240). At startup (ovl28, FUN_1943_0003) it raises N in steps of 40h until
  the centred stick fits, then uses N = (that + 100h) * 2 = 480h and takes the centre; detected once,
  so a stick is always reported (still and centred: the game posts joystick events only off centre
  or on button changes). A reading of N later means "unplugged" and turns it off for good.
  Its poll is a hook (1cfc:0018): 1bbd:0184 in menus (buttons only), 13be:087b in flight (posts
  event 6 with x/y -8..8 at DS:00C9/00CB, buttons DS:00CD); flight takes x into DS:C3AC (2acc:39fc),
  the mouse's into DS:00EF too. Checked headless: `%100.0` gives +8, `%-100.50` -8.
- The user's stick is a Logitech Extreme 3D Pro (HID joystick, not an SDL gamepad: 4 axes, 12
  buttons, a hat). With SDL's MFi (GameController framework) backend on, SDL saw no joystick at all,
  even with a window; with `SDL_HINT_JOYSTICK_MFI` "0" the IOKit backend reports it. The host
  handles plain joysticks (SDL_EVENT_JOYSTICK_*) as well as gamepads; the page takes
  non-standard-mapping pads too.
- Roll: the keys `,`/Ins (0x33/0x52) and `.`/Del (0x34/0x53) step DS:C3AE (2acc:39fe) towards -9/+9
  with every make code (held keys ramp up through the typematic repeats); joystick button 2 held
  turns the stick's x into the roll directly and its y into speed changes. The host repeats the
  hat's left/right as `,`/`.` every 70 ms.
- Twist (a gamepad's right stick x; a stick's twist only with `WC_ROLL_AXIS=2`, the page `?twist=5`
  in Chrome, 2 otherwise: off by default, since the user rolled too much by accident while steering;
  the hat's left/right rolls instead): the PC joystick has no such axis, so the runtime writes the roll rate (1..9 either
  way) itself. Each flight frame, FUN_13be_1567 reads the input (0d0d, which zeroes the rates when
  idle, and whose joystick event cancels key roll) and then turns the rates into turning speeds
  (13be:0c4e). WcRecomp emits `wc_hook(addr)` before the instructions cli.ts passes (the game's
  `rates`), and the hook writes DS:`roll` there (SM2: 13bb:0c08, DS:C694). `WC_KEYS` `%x.y.r` twists.
- There is no reachable joystick calibration: WC.EXE has the texts ("Center Your Joystick ...",
  "UPPER LEFT", "LOWER RIGHT", DS:2A8F) but no code refers to them; the game only takes the centre
  at startup (FUN_1943_0003).

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
- `XLAT` reads linear BX+AL, without any segment (DS or the prefix's); the generator writes it itself.
  Miles's AdLib driver maps note velocities through `cs: xlat`: every voice got 0 from the interrupt
  vectors, every carrier total level 63, and the music played inaudibly (peaks ~100).
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
  recognised at runtime by their instruction bytes. A write into those bytes marks the driver
  stale and prints a report (the first time; WC_DUMP saves the memory); the next call into its
  segment checks the bytes again and drops the registration if they changed. It used to stay
  registered for good, so code at that segment ran the old driver's recompiled code: WC2 hung.
  `discover` refuses to recompile a "driver" without the zero word that starts every driver.
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
- `npm run recomp` checks that the generator finished: analyzeHeadless exits 0 even when a script
  throws, so `WcRecomp.java` writes `summary.json` (`summary_<driver>.json` for a driver) into the
  recomp folder last, and `cli.ts` fails without it, on more unhandled p-code ops than are known
  (`unhandled` in `GAMES`: SM2.EXE has 9 in data decoded as code), or on a hook (`hooks`, `rates` in `GAMES`) that isn't at an instruction.
- Division: every `INT_DIV`/`INT_SDIV` comes from `DIV`/`IDIV`, whose quotient must fit in half the
  dividend (`udiv`/`sdiv` get its size: DX:AX 4, AX 2). The 8086 raises INT 0 when it doesn't (the
  C library's "Divide error", and the game ends); here the quotient is truncated and the
  instruction reported once ("wc: divide overflow at"); `WC_STRICT=1` stops there.

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
  the game folder. A read or write whose buffer (DS:DX, up to 64 KB) runs past 1 MB goes on at 0, as
  with the A20 line off (`transfer`, reported once; `WC_STRICT=1` stops), instead of past `M`; the
  DTA's fields go through `mwr`/`mrd` (wrapped at 1 MB, code writes noticed).

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

## Sound (runtime/opl.c, mt32.c, sb.c)

- AdLib: OPL2 at 388h (index; reading it gives the timer status) and 389h (data), emulated by
  Nuked-OPL3 (third_party/, submodule, LGPL 2.1) at its native 49716 Hz. Samples are made on the
  emulation thread in `opl_poll()` up to the current real time (also right before each register
  write), and pushed into an SDL3 audio stream (60 ms prefilled; cleared if over 0.25 s queued).
- `WC_WAV=file` records the sound, also headless: the intro's music starts after ~3 s, peaks
  around +-1800 (quiet, like the chip's raw output).
- The music card is the config's (or the arguments') word: `a<port>` AdLib (`a904` = 388h,
  DS:772c = 2), `r`/`R` Roland (1), `p`/`P` (3); arguments come after the config's words, so the
  last one wins (main.c appends `r` with WC_MT32). The driver is entry (card - 1) of `strax.drv`
  (FUN_19c5_14f8); the Roland one is d9000 (SM2.EXE loads it unchanged: same hash; it needed its
  own discover round, like every driver, since each program has its own recompiled drivers).
- Without a game folder the runtime used "." and the game ended in "Abnormal program termination";
  main.c now checks that the folder has the program's EXE.
- MT-32 (runtime/mt32.c, munt's libmt32emu, submodule, LGPL 2.1+, C interface, static; the
  project is C and C++ for it): WC_MT32=ROM folder, WC_MT32_MACHINE (munt's machine names). The
  driver never enters UART mode: after a reset (FFh) it sends every message in intelligent mode
  behind D0h ("want to send data"; DFh before SysEx), so data bytes go to the synth in either
  mode. It doesn't wait for interrupts. The synth renders even when nothing hears it (quick start,
  no output), or its MIDI queue would fill. Only one device opens the sound (AdLib otherwise);
  sound.c has the output and WC_WAV. The intro's music starts after ~10 s, at 32 kHz, peaks near
  full scale (a few clipped samples, as munt makes it).
- In the browser there is no MT-32 yet (the page would need the ROMs as files, apart from the game's).
- Sound Blaster (runtime/sb.c, `WC_SB=0` off): DSP at 220h, IRQ 5, DMA 1 (fixed: what wc2.cfg's
  `c25 d1` and GOG's DOSBox say), an 8237 for the 8-bit channels. Written from Creative's "Sound
  Blaster Series Hardware Programming Guide" (not in the repository: its licence forbids copies).
  A block plays in emulated time; its end advances the DMA channel and raises IRQ 5 in
  `sb_poll`, which `poll_devices` calls after the sound up to now is made. `sound_out` gets each
  block's emulated time and `sb_mix` adds the samples (linear interpolation, full scale +-16384) to
  the AdLib's or MT-32's; DSP writes first make the sound up to that moment, like OPL writes.
  `WC_TRACE=1` prints each block. WC1 and SM2 have no digital sound (no speech pack for WC1;
  "Error loading pilot speech" is about the wingmen's comm texts).

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
- The default is per program (`cycles` in cli.ts' GAMES, wc_program.cycles): WC2 at 12000, where
  8334 felt sluggish, 14400 "a lot smoother", then 12000 better still. Privateer at 300000 (see there). Headless, the instruction
  clock runs at that speed too: tick timings depend on it (fewer instructions per tick at 12000).
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
- Headless, the clock counts instructions (pc.c, `WC_CLOCK`; `real` for the old behaviour): emulated
  time = instructions / speed (WC_CYCLES or the program's default; "max" means the default here) +
  time skipped in waits (pc_frame's slot in flight, pc_wait), no throttle, no quick-start warp. Runs
  are deterministic (same instruction count and picture every time) and fast: WC2's intro to the
  cockpit in ~5 s instead of ~5 minutes. `WC_KEYS` entries are sorted by tick, and `tick:quit`
  ends the run (report, WC_SCREENSHOT), so a screenshot at any tick takes seconds.
- The WC1/SM2 `WC_KEYS` sequences in this file were made on the real clock at full speed
  (`WC_CYCLES=max`): use `WC_CLOCK=real` with them, or re-time them (cheap now). In flight the
  instruction clock charges heavy frames their instructions at the set speed, unlike real time.

## State

The intro runs natively (~400–600 M instructions/s); a frame-buffer screenshot shows the credits.
The SDL3 window shows it live, takes the keyboard and mouse, and plays AdLib or (WC_MT32) MT-32
music, and WC2's speech on the Sound Blaster. Missing: text mode, the MT-32 in the browser. A joystick or game controller is the PC joystick.

Wing Commander II (`WC2.EXE`, `--game wc2`, see below) runs through the intro, the menus, pilot
creation, the quarters and flight (first mission: no missing code in a scripted flight). `npm run play` is discover in the
window (no time limit, normal speed, restarts after each recompile).

The Secret Missions 2 (`SM2.EXE`) run too, built the same way with `--game sm2` (see below).
Privateer (`PRCD.EXE`, `--game priv`, see below) runs through the intro and the main menu, a new
game and from Achilles into space (headless, scripted), with Origin's EMS driver JEMM emulated.
The same runs in the browser (`npm run web`, see below). Sound via Nuked-OPL3 (LGPL) on 388h/389h and libmt32emu (LGPL) behind an MPU-401 at 330h/331h —
not DOSBox Staging's audio code (GPL, tied to its mixer).

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
  Alt-X in the main menu (`2700:+alt,2710:+x,2716:-x,2720:-alt,2900:+y,2906:-y`) quits; the exit
  needed 11711, 116ee and 104a0, and ends in DOS 4Ch with AL=87h (exit 135; WC.EXE's menu ignores
  Alt-X). Continue Campaign is at 60,115; a sleeper at 137,90 asks "(Y/N)" ~700 ticks later. Esc in the
  briefing goes back to the barracks; Space steps through it into flight. WC_KEYS takes up to
  1024 entries. Discovery: `discover --game sm2 --seconds 900` with such keys (and `--exe` in a
  scratch copy of the game folder holding a save from the mission tool) found nothing missing
  through intro, protection, menus, simulator, rec room, barracks, briefing and a mission flown
  (autopilot key, no combat steering) back to the barracks.

## Wing Commander II (WC2.EXE, ../game2)

- GOG release (SHA-256 0eb4e54e...), `--game wc2`: folder `../game2`, output `out/wc2/`
  (`out/wc2/bin/wc2`), `entries-wc2.txt`. Borland C++ 2.0 (1991), VROOM overlays again but 124 of
  them (796 entries); DGROUP 1976h (runtime DS = load seg + 1976h = 2376h), entry 0000:0000.
- SO1.EXE/SO2.EXE (Special Operations, `--game so1`/`so2`, `out/so1/`, `out/so2/`): the same engine,
  folder, load segment and speed, with other addresses, so their own `entries-so*.txt`. They load
  WC2's drivers with other header values and data: drivers.txt maps those hashes to d7000 and db000
  (the code is the same; compare captures before letting `discover` name a new driver, which also
  leaves too few segments). WC2's driver entries were copied into the entries files, and its
  program entries where 16 bytes of code match in just one place.
- SO2's scene interpreter (ovl36) checks its big switch as `cmp bx,n; jbe +3; jmp default; shl bx,1`,
  the default too far for JA: `switchTargets` knows that now. Without it the 186 cases came up one
  discover round at a time. Nothing else in the other programs has that form (their output didn't change).
- Scripted (instruction clock, WC_MOUSE=1): Start New Game as in WC2, at 2200 in SO1 but 3000 in SO2
  (its menu comes later); Esc 400 ticks on, Space every 200 from 800 on to 14700; the database by
  14000, ready for keys by 15000. Keys there must be held ~10 ticks (`+c`, `-c`): a press and release
  at once goes unnoticed. New pilot: `c` at 15000, names from 15600 each 400 ticks. Transfer:
  `15000:@110.60` + a click (the T key did nothing), a pilot row `16600:@100.44` + a click: the
  quarters straight away, no name asked. The middle door by 17300 (a click at 160,100 earlier hits a
  database button and plays the credits), the cockpit by 30000. The console (25,110) has Load 110,58,
  Save 110,78 (a slot row, a comment, Enter). The saves are savegame.so1/.so2; a lost mission asks
  Replay (left) or Continue Script (235,168).
- One overlay relocation word has bit 0 set (overlay 77, 05B1h): stub index 182 | 1 means the
  segment the overlay's own code is loaded at, not its stub (a far pointer to a callback in
  itself). `flatten.ts` maps it to the overlay's flattened segment.
- The flattened image ends at 6BAE0h, past DRIVER_VIRT_SEG (6000h): code there was taken for
  driver code ("missing code" for a function that was recompiled). Drivers are now looked up by
  their actual virtual segments (`driver_code()` in cpu.c), and `discover` gives new ones a
  segment above the image.
- Its drivers differ from WC1's except the flight graphics driver: same code as d7000 (only the
  header words differ), so drivers.txt maps its hash to d7000 and FRAME_COPY applies. da000 is
  the AdLib driver, db000 the menu/room graphics. dc000 is its own Roland MT-32 driver (not WC1's d9000;
  found with `WC_MT32`, since each config loads other drivers); speech plays over either music. WC1's d7000 entries were copied into
  entries-wc2.txt (valid below 7565h, where the two captures start to differ).
- Borland's switch on a long (`mov cx,N; mov bx,table; cmp cs:[bx] ...; cmp cs:[bx+2N] ...; jmp
  cs:[bx+4N]`, four-character tags like "FORM") is read by `sparseSwitchTargets()` too.
- Keyboard: the game asks INT 15h AH=C0h for the BIOS configuration. With feature bit 10h (INT 9
  calls the keyboard intercept INT 15h AH=4Fh) it hooks INT 15h, records the key and chains on, so
  the BIOS buffer still fills; otherwise it hooks INT 9 and swallows every key, and the pilot name
  entry (Borland's kbhit/getch, DOS AH=0Bh/07h) never sees one. The runtime reports the bit (an AT
  configuration table at F000:E6F5, as DOSBox) and its INT 9 calls INT 15h AH=4Fh when hooked.
- `wc2.cfg` is `v a904 c25 d1`: AdLib music, Sound Blaster at 2x0h with x = 2, IRQ 5 (`c25`), DMA 1
  (`d1`). GOG's folder has the Speech Accessory Pack (SPEECH.Snn for the cutscenes, COMMUNIC.Snn
  for the wingmen in flight): "Speech Enabled." is printed when the card is found. Its Sound
  Blaster code is in WC2.EXE (16cb), not a driver: reset/AAh, time constant A3h (10752 Hz), D1h,
  single-cycle 14h blocks of 4000 bytes, double-buffered at 4BBD0h/4CB70h, DMA mode 49h; the IRQ
  handler (16cb:028f, an entry) acknowledges at 22Eh and starts the next block. Only speech is
  digital: effects stay on the AdLib (the cutscene interpreter, ovl37 opcode B0h, and the comm
  messages, ovl06, are the only callers).
- Scripted (instruction clock, WC_MOUSE=1): the main menu by 2100 ticks; click "Start New Game"
  `2200:@160.135,2250:+lmb,2256:-lmb`; Esc at 2600 skips the Kilrathi scene to Tolwyn's office;
  Space every 200 ticks (3000-8000) steps through the opening; the personnel database by 9000,
  `c` at 9600 (earlier is missed; a later Enter then picks "Transfer"), names typed letter by
  letter 40 ticks apart from 10200, 10600, 11000 (first, last name, callsign, each + Enter) reach
  the quarters by 11300. The middle door (`11900:@160.100,12000:+lmb`) is "Fly mission": the
  flight deck scene, then the cockpit by 18000. These were made at 14400 without speech
  (`WC_SB=0`). With speech the dialogue takes longer: at 12000 the database comes at ~12400, and
  everything from `c` on goes 3600 ticks later (`c` at 13200, the cockpit by 21600; checked). The airlock (80,80) quits ("Leaving Wing Commander 2").
- The game writes savegame.wc2, TEMPGLOB.000, MEM.$$$ and err.$$$ in GAMEDAT: test in a copy of
  the folder (`--exe copy/WC2.EXE`).
- Loaded at segment 0A00h (32 KB higher than WC1, `loadSeg` in cli.ts), as GOG's DOSBox runs it
  (`loadfix -32 wc2.exe`). SHIP.V18's header holds a stray far pointer, 3831:772E, which goes
  into the ship type table (DS:14BC, 0F3h per type, sprite at +0EBh), then into an object's
  sprite slot (DS:4A24), and is freed when the sprite changes (FUN_1bd7_259a). The game's free
  (FUN_25ad_0add) releases the heap block containing the address; loaded at 0200h, linear 3FA3Eh
  is in the first block, the menu graphics driver db000, so in a mission's cutscene the heap
  handed its memory out (FileChunks) and calls into it hung or stopped. Loaded higher, the
  address is below the heap and the free does nothing. The heap (FUN_25ad_*): all free
  conventional memory, linear base/size at 344f:31c0/31c4, a table of 8-byte entries (linear
  address, size, flags 8000h used, 4000h kept by the purge) from 344f:31c8 up to the end.
  Pointers with segment C000h-FFFFh are the game's EMS heap (FUN_2537_*).
- Saving checks for savegame.wc2 with DOS find first (AH=4Eh); when it reported nothing found
  (the runtime's old stub), every save created a new file of 9 blank slots (1672 bytes each, slot
  8 the current game) and wrote only its own slot, so earlier saves vanished. dos.c now implements
  4Eh/4Fh (8.3 names, wildcards; the search in the DTA's reserved bytes, one folder at a time).
  Saving by script: the console at 25,110 in the quarters ("Save/Load game"), a slot, S, the
  slot (arrows, Enter), a comment, Enter (checked twice into slots 0 and 1, both kept).

- Pilot transfer (ovl14, personnel database, `t`): DS:820b lists savegame.wld twice and crusade.wld;
  the slot must be used and its campaign byte (+1AAh) match the choice, nothing else. Taken from the
  slot's player: last name, callsign, missions, kills; rank is always 2 (Captain), the first name is
  asked. An empty path isn't accepted; DOS paths stay in the game folder, so the save goes in a
  subfolder (`wc1` means GAMEDAT\wc1\: no backslash to find on the host's layout). Enter is also the
  left mouse button (FUN_20e6_0004; keypad = pointer, Ins = right), and while it's held the mouse
  poll (BX=0) and this poll flip the button at every poll, each flip posting a press. In the pilot
  list, with the pointer placed off the rows, a press means cancel: with instant file access it
  closed at once. DOS AH=4Eh in another folder now takes 300 ms (`pc_delay`), so the key is up.
  Selecting by key also leaves the character in the BIOS buffer, hence in the next text field (`c`
  in the path, a digit in the first name): click. Scripted (12000, instruction clock, WC_MOUSE=1):
  the database by 14000; `14100:+t,14106:-t,14500:@120.85,14550:+lmb,14556:-lmb` (Crusade), `wc1` +
  Enter from 15000, pilot row 1 `15450:@120.44,15500:+lmb,15506:-lmb`, first name from 16000: the
  quarters ~600 ticks later. Checked by saving (savegame.wc2: 58 missions, 594 kills) and into flight.

## Privateer (PRCD.EXE, ../game-privateer/game)

- GOG's Mac package (`wing_commander____privateer___enUS_1_0_33062.pkg`): `pkgutil --expand-full`, the
  game in `package.pkg/Scripts/payload/Contents/Resources/game/`, the CD image `GAME.GOG` (ISO 9660,
  `7z x`: PRIV.TRE, RF.TRE, PSFONTS.CAT/INF, X.LBM). The game folder holds both: DOS drive letters
  are ignored (dos.c), so `PRIV.CFG`'s `=D:priv.tre` finds PRIV.TRE there. The game never asks for
  MSCDEX (no INT 2Fh). `--game priv`, output `out/priv/`, `entries-priv.txt`.
- `PRIV.EXE` is a loader (386 and XMS check, installs Origin's EMS driver JEMM from JEMM.OVL, runs
  prcd.exe; refuses when an EMS driver is there). The game is `PRCD.EXE` (GOG SHA-256 84d7b3b3...),
  Borland C++ 3.x (1991 library), 183 VROOM overlays / 2204 entries, DGROUP 6E4Dh, SS:SP 750Eh:0080h,
  loaded at 0200h (DS = 704Dh). 386 code in real mode throughout (66h/67h prefixes, FS/GS, MOVZX,
  SHLD...): Ghidra's real-mode language and the generator handle it (R[] holds EAX, FS 108h, GS 10Ah).
- Flattened, the overlays reach E50B0h at 1000:0000: more than conventional memory. `flatten` allows up
  to F000:0000; `main.c` puts only what is below A000:0000 into memory (the overlays there are just
  the initial heap), all of it into the pristine copy. 20561 relocations (relocs.bin up to 65536).
- No room left for drivers below 1 MB: their virtual segments go past FFFFh (d1f000, d2f000...):
  Ghidra imports them at F000:0000 (the low 16 bits), WcRecomp moves their code addresses (F_, L_,
  P_, 0x... with 5 digits; constants carry ULL and stay) up by the rest, and the program's recompile
  skips entries past 1 MB.
- JEMM (ems.c). The game checks it with INT 15h AX=1209h BX="AC" (AX=0, BX=1209h back) and drives it
  with more such commands (BX = two letters, low byte first): "MS" switches into JEMM's mode, "ms"
  back to plain real mode (BX = whether it was on), "FF" gives ES:DI = the mode byte (0060:0000
  here, below 0280h: the game's count of low memory pages to map with the non-standard INT 67h
  AX=5002h comes out <= 0 and it skips that), "FS" sets its divide error handler (636b:0016),
  "SG"/"RS"/...: AX=0. In JEMM's mode the EMS page frame is A000:0000 to the end of the first MB,
  24 pages (AH=41h says A000; AH=44h/50h take pages 0-23): 18 hold the game's far heap, 6 are
  windows. Not B000 (with the HMA): the game normalizes frame pointers to seg = linear >> 4, which
  wraps above 1 MB. Plain real mode has the usual 4-page frame at E000. The game spends its time in
  JEMM's mode; it switches to plain real mode for DOS file reads into EMS (156a:01c8) and the BIOS.
- The VGA memory in JEMM's mode is the HMA: the frame is below B000h, so the game makes its screen
  FFFF:0010 (5f91:0588, FUN_61ad_019a with 0x10ffff), linear 100000h, and copies its frames there;
  JEMM pages it onto the VGA. The A20 line is on (`wc_amask`, wc_rt.h: M is 110010h bytes, lin()
  and mrd/mwr mask with it; set at the first JEMM call; with it off, clearing the screen wrapped onto
  the interrupt vectors).
- JEMM runs hardware interrupt handlers in plain real mode (`ems_irq()` around IRQs in pc.c and the
  mouse handler; back to JEMM's mode after). There the game's EMS code sees the mode byte 0 and maps
  through the 4-page frame without touching its own record of JEMM's 24 pages (DS:68a4), and the
  handlers' AH=47h/48h (the sound sequencer: ~500 pairs a second) save and restore those 4 pages.
  In JEMM's mode they took windows in the 24-page frame and either the game's record went stale (48h
  restoring all 24: "Error #1911" on a TRE read, or fatal 1509) or the interrupted code's windows
  changed under it (4 pages: the heap check, "Error Code: 1407"), in most runs.
- The game maps one EMS page into two physical pages at once (a heap page and a window: 268000 times
  before the menu), which copying pages in and out can't keep in step: from the first JEMM call on
  (natively) the EMS store, the upper memory's real contents and the HMA's are one shared file, and
  every 16 KB of A0000h-10FFFFh is an mmap(MAP_FIXED|MAP_SHARED) of the page it shows (M is 16 KB
  aligned; 16 KB is the Mac's page size and EMS's). Switching modes remaps the 28 slots. Not before
  that call: WC1's two-player view forks, and must not share memory with the game. The web build
  keeps copying (aliases go wrong there). `ems_vram()` (vga.c, screenshots) is the real VGA memory.
- The SB driver plays straight out of the EMS frame (F0000h-FBFFFh in JEMM's mode): it asks VCPI for
  each page's physical address (INT 67h AX=DE06h) and programs the DMA with it. Answered with the
  linear address, the DMA read whatever that address showed while the sound was mixed, the BIOS area
  in plain real mode (every interrupt): the sound crackled, ~22000 jumps of a third of full scale in
  a minute. `ems_phys()` now answers with the page's place in the EMS store (from 200000h, through
  JEMM's map, whatever the mode), and sb.c reads the DMA through `ems_phys_byte()`. DE09h (the debug
  registers, its "range watch" debugging) answers success.
- Between single-cycle blocks (the next starts in the interrupt handler, a few samples later) the
  emulated DAC dropped to 0, a click each block; it holds the last sample now, fading over 20 ms (a
  held level stayed as an offset after the driver's 4-byte test block). `tools/` has no WAV check;
  counting sample jumps over 6000 in a WC_WAV recording showed both.
- DOS memory (dos.c): the game shrinks its block (AH=4Ah) and allocates the rest (AH=48h BX=FFFFh,
  then what is offered); 48h/49h/4Ah now keep a list of blocks below A000h (the program's block alone
  still always resizes, as before, for the other games).
- Code on the stack: Borland's int86() writes `push bp; int n; pop bp; retf` into its frame and
  far-calls it, the EMS calls `mov bp,[bp-22h]; int 67h; retf 2`: `int_thunk()` (pc.c) runs such code
  when CS = SS. discover took the second for a driver once (DGROUP starts with a zero word too): an
  Origin driver now needs its first table entry >= 200h.
- Generator: Ghidra reads `CALLF [BP-4]` through DS (int86 again); a BP-based operand without a prefix
  now uses SS (29 in PRCD.EXE, 2 in WC.EXE). The sound code jumps with `push dx; push ax; retf`
  (a function table lookup): a RETF right after two pushes (or one 32-bit push), or a RET after one,
  is a jump to what was pushed.
- Near calls through variables (the sprite code picks its decoder: `mov word ss:[4938],0470h` ...
  `call ss:[4938]`, and `mov di,[46e0]; add di,4222h; call ss:[di]` through a table in DGROUP):
  `storedCallTargets()` seeds them in the program (Ghidra's functions and the fragments, repeated
  until nothing new turns up; 65 in PRCD.EXE, 1 in WC.EXE). They had come up one random run at a time.
  Also stored from a register set by a branch, `mov ax,06e5h; cmp bl,bh; jb; mov ax,0a47h; mov
  ss:[493a],ax`, then `jmp ss:[493a]`: every `mov reg,imm16` up to 5 instructions before (16 more;
  play stopped at 68427 and 69da3, two of them, after the sweep).
- Borland C++ 3's switch on a long in 386 code: `mov cx,N; mov bx,table; mov eax,cs:[bx]; cmp
  eax,..; jz; add bx,4; loop; jmp default; found: add cx,cx; add bx,cx; jmp cs:[bx+2N]` (N dwords,
  then N targets): `sparseSwitchTargets()` reads it (accepting a mission stopped at a73ac, ovl31).
- Sweep (`sweep: true` in GAMES, WcRecomp's `sweepTargets()`): every stretch of the code segments no
  decoded instruction covers is decoded from its start, and becomes an entry point when it is at least
  3 ordinary instructions up to a return or jump inside the stretch (no x87, system, BCD or other
  instructions data turns into). Before it, 97.6% of the code bytes were decoded (19.7 KB left in 2109
  stretches, mostly under 8 bytes: padding and data) and play stopped for missing code one restart at
  a time; it adds 879 entry points (library routines after a `ret`, branches nothing analysed jumps
  to), 98.7% (11 KB left: AIL's variables, the 3D code's tables). Function prologues after a return
  found only 9: what was missing were mostly targets inside functions. Only Privateer has it, so
  the other programs' recompiled code stays as checked.
- Code reached through pointers: `entries-priv.txt` starts with the far code pointers in the data
  (relocated segment words naming a code segment, the offset before them: vtables, the startup
  tables, callbacks), less those into stub headers or data (they decoded into x87 and system
  instructions); then what discover found.
- Sound: `SOUND.CFG` is `music effects` (GOG: `3 1`), then port/IRQ/DMA lines for each (816 = 330h;
  544 = 220h, IRQ 5, DMA 1). Music 1 and 3 load ROLAND.DRV (3 without sending the MT-32 timbres,
  probably General MIDI), 2 ADLIB.DRV; effects 1 SB.DRV. They are Miles Design AIL 2 drivers from
  PRIV.TRE (..\..\DATA\SOUND\): the first word points to (function number, offset) pairs ended by
  FFFFh, then "Copyright (C) 1991,1992 Miles Design". discover and the generator take that header too.
- The Roland driver waits for the MPU-401's acknowledgement with interrupts off: the MPU at 330h is
  now always there (mt32.c), as in GOG's DOSBox; without WC_MT32 its MIDI goes nowhere. main.c
  doesn't add WC1's "r" for WC_MT32 here: SOUND.CFG picks the card, and "r" is Righteous Fire.
- GOG's music 3 is General MIDI, which its DOSBox sends to the host's synthesizer (`mididevice`);
  there is none here, so the game played no music at all. Without WC_MT32, dos.c (`sound_cfg`) gives
  the game a copy of SOUND.CFG whose music is 2 (AdLib); the file stays as it is. Not 1: GOG's
  PRIV.TRE has the music as .ADL and .GEN only, and the MT-32's (COMBAT.ROL) is missing ("Program
  halted by code"); with WC_MT32 the MT-32 gets the General MIDI as before. The AdLib needs its port too (904 = 388h on the music's line): at 330h the game says "Your Privateer
  sound configuration is incorrect" and exits with 1. ADLIB.DRV is d3f000. Its detection starts OPL
  timer 1 (80 us) and reads the status port 200 times, ~1 us each on the ISA bus but a few
  instructions here: each read of 388h moves the OPL timers' clock on by 1 us (opl.c, `io_ns`).
- Joystick: GOG's JOYA.DAT calibrates the stick to 0..80h, centre 40h, counted in reads of port 201h
  like WC1 (joystick.c); the emulated stick's centre of 256 reads was full right and down, and the
  game moved the pointer there all the time (mouse clicks never landed). `stick: 64` in cli.ts'
  GAMES (wc_program.stick) is Privateer's centre.
- Mouse: the game never polls AX=3; it installs an event handler (AX=14h, mask 3Fh, 71db:000b) and
  sets the position (AX=4) all the time. mouse.c calls handlers (AX=0Ch/14h) between instructions
  when the host's mouse changed and interrupts are on (irq_poll), with the buttons as they are (the
  "clicked" latch that AX=3 clears would hold a release back forever).
- DOS AH=2Bh/2Dh (set date/time; the game sets the time on exit after its timer ran fast): ignored.
- Speed: GOG's DOSBox runs it at `cycles=max`, and the game paces itself by its 60 Hz timer (frames
  wait on the counter at DS:5493), so more instructions only take stalls away. At 12000 (WC2's) the
  intro's 3D scenes (the ship turning on the pirates) stalled ~0.5 s at a time; at 100000 they still
  fell to ~8 fps; from 200000 on every frame keeps the game's pace (~40 ticks in the intro, 21 = 28.6
  fps in flight). The default is 300000 (an M4 runs ~900000 instructions/ms). Frame intervals came
  from WC_TRACE=1 (IRQ0 lines) and WC_WATCH=107d00 (a pixel of the screen in the HMA: one write a
  frame). Headless runs take longer now (25 times the instructions per tick).
- Runs differ: the game seeds its randomness from the clock (DOS time), so the instruction clock
  doesn't make it deterministic. The timer runs at 600 Hz (AIL), so WC_KEYS ticks are 1/600 s.
- Scripted (instruction clock, WC_MOUSE=1, the scratch folder): the intro (cockpit scene, ...) and the
  main menu by tick 95000; `N` held 30 ticks at 100000 starts a new game ("register your new
  Quine 4000"); a name and a callsign typed from 103000 (keys held 30, 150 apart, each + Enter)
  reach Achilles' landing pad by 108000; a click on the ship (`116000:@160.110,116300:+lmb,
  116400:-lmb`, "Launch") the cockpit in space by 125000.
- The game writes into its folder (a new game: PLAYTYPE.IFF, DELTSECT.IFF, PLAYSCOR.IFF; saves; the
  options): test in a copy (`--exe copy/PRCD.EXE`; ../game-privateer/test has symlinks to the TREs).
- Righteous Fire (`priv r`, RF.TRE, RF.CFG; GOG's launcher runs `priv r`): PRCD.EXE gets the arguments
  after the game folder (`out/priv/bin/priv <folder> r`); it opens rf.cfg and D:rf.tre. Privateer's
  script works unchanged (same menu, Quine registration, a landing pad at 110000, launch at 116000):
  a new game reached the concourse (click 280,100 on the pad) and space, nav map included, with no
  missing code. Missions, combat, landing and saving there are untried. RF's Load lists `????????.PRS`
  (find first); a Privateer .SAV copied to .PRS loads, but New Game is the way in: it takes a Privateer
  pilot (the user's H5: same KILL and SCOR, other guns, armour 3000 instead of 500, other credits, plot
  zeros). The header holds the plot state at 31h: a mission like `s5md`, `FFFFFFFF`, or zeros in RF.
  Then a flags byte (ovl03_07ce packs the plot object DS:6569's +0Bh and +0Ch): bit 1 ended, bit 0
  succeeded (both needed to be paid when landing, ovl04_0589), bits 2-6 requirements checked, met,
  paid, taken (its effects applied), success checked; bit 7 is junk. 7F/FF: done; 00/80: not taken
  yet (the fixer offers it). The next word counts the missions in the log. Landing during the user's
  Goodin 3 (RF, s10mc) left A2 (ended, not succeeded, taken) and no mission: set to 80 (Goodin offering it again unchecked).
- Self-firing turrets (runtime/turret.c, a hook after the player's guns at 5b2b:048c in FUN_5b2b_0158,
  where [BP+6] is the weapons and SI the world): only turrets whose guns the player switched on in
  their view (the user's choice; each gun's +35h stays as set there, the cockpit view doesn't clear it,
  and a loaded game starts with every gun off). Every second, if not in a turret view, the nearest
  hostile ship in a turret's arc and range is aimed at (yaw/pitch, the matrix by ovl134_0000/0243 as the
  turret view makes it), the view (DS:143C, 4/5 = turret 1/2) and the weapons' current turret (+3Fh) set,
  and FUN_589b_0004 (the player's guns: those on, of the current turret) called; then all restored.
  Ships are the objects with the player's vtable in the list at DS:66D0 (node: next, object; position
  longs at +14h, 256 = 1 m); hostile is the radar's test, FUN_2089_078e(pilot's faction +13h, player
  DS:66B4) != 0 (pilot = vt+30h, then its vt+8). Turret objects (ovl135): DS:143D/143F, matrix at +4
  (rows right, forward, up; forward = the shots' direction), place +28h (the Centurion's: 120 m behind),
  yaw +34h, pitch +38h (256 = 1 degree; positive yaw turns the rear turret towards the ship's +x),
  limits +3Ch..+48h, fitted +4Eh. Guns: +0 speed, +4 life (256 = 1 s: plasma 940 m/s, 3.4 s), +22h the
  turret (0 front), +35h on. The shots keep the ship's velocity. Checked headless against hostile Talons
  (a test copy of H5 with Militia at -300): at ~2 km about half the shots hit (they end early); from
  3 km all missed, the Talons changed speed during the 3 s flight. At exit (atexit) a report counts
  hits: every turret shot is followed (the objects new after the call), and one that ends before 90%
  of its life within 150 m of a ship hit it; a ship hit that leaves the object list within 5 s was
  destroyed (one Talon in the test). A shot's memory is reused at once, so a 300 m jump ends it too.
  Aim (checked): the game's matrix for any yaw and pitch matches the intended direction within the
  1/256 resolution; the two plasma guns' shots start ~15 m either side of the turret and fly ~920 m/s
  relative to the ship (not quite 940). The current frame's time (DS:2768) is this frame's movement:
  speeds from one frame's difference are exact. Misses are the fighters weaving: a lead with half or
  all of their acceleration didn't help; holding fire while 1/2 |a| t^2 (t the shots' flight) is over
  60 m did: three scripted fights (A/B/C: H5 with Militia hostile, turning differently) went from 45%
  to 71% of the shots hitting, with as many hits. The user's own first count was 20% (106 shots).
  Then, in the user's fights: 50% (56 shots), 59-60% from under 1.5 km, 3 of 14 from farther: the
  range is 1.5 km by default (two thirds of the guns' reach if less), at the user's request.
  Pace: a turret fires whenever one of its guns has recharged (energy +2Fh up to its cost +2Bh, 256 =
  1 s: plasma 0.72 s, tachyon 0.40 s), not once a second as at first (user's wish). Guns (from
  memory): plasma 940 m/s, +29h 72 (copied into the shot, probably its damage), ship energy +33h 19;
  tachyon 1250 m/s, 50, 8. The three fights with the save's turret plasma made tachyons (WEAPGUNS
  type 6 -> 7, a test copy): 39 hits from 38 salvos against plasma's 25 from 28.
  Charge: when the ship's part at +6 says so (its method 28h), a shot takes cost * 256 / that part's
  word +32h (FUN_5b2b_0009 refills up to it, FUN_589b_0004 fires from it): RF's gun cooler, the save's
  COOL chunk (the user's: 320, so 102 of 128; also SHBO 320, SPEE and THRU 300, RF's other upgrades). turret.c's own check used the plain cost, so after the first salvo the
  turret waited forever ("11 salvos in a lot of missions"); fixed, the three fights (an RF copy of H5
  with Militia hostile) went from 1 salvo each to 10, 12, 13; Privateer's unchanged (28 salvos, 25 hits).
  At every launch FUN_448e_0105 builds the player's ship and FUN_592c_010c copies the guns from its
  record (ovl80_00d7, +35h from the record: 0), so the switches are lost: turret.c remembers, per turret,
  the guns left on in its view and switches them on again when they turn up all off (`WC_TURRET_CLEAR=
  frame` clears them for testing). The exit report has the time with guns on, with a hostile in range
  and in the arc (the Centurion's rear turret: yaw and pitch -60..60).
  The Galaxy has two turrets, top (F5, pitch -10..50) and bottom (F6, -50..10), with yaw limits 0..0: its
  INFO chunk has no yaw limits (ovl135_0000 sets 0, 0), and equal limits turn freely (ovl134's turning).
  turret.c took 0..0 as straight ahead only and never fired; checked with the user's GALAXY save (Militia,
  k=2/4/5 of PLAYSCOR, at -300): both turrets fire, 20-29 salvos in three fights, 40-73% hit.
  Range by the shots' flight time (1.6 s, plasma's 1.5 km; at least 1.5 km, at most 2/3 of the reach),
  at the user's request: RF's 1350 m/s guns 2.15 km. The three RF fights went from 35 salvos and 27
  hits to 94 and 69 (from 1.5-2.5 km 42 of 70 hit, as from nearer); Privateer's plasma ones unchanged.
  Then 3.5 km for RF's guns (user: hostiles fired from just outside 2.15 km and the turret stayed quiet):
  1.5 km + (speed - 940) * 2000/410 m (tachyons ~3 km), at most 80% of the reach; the fights went to 144
  salvos, 116 hits, 5 kills (from beyond 2.5 km 18 of 28 hit). In the user's flights from 3-3.5 km only
  1 of 38 hit (2.5-3 km 32 of 63), so 3 km: 1.5 km + (speed - 940) * 1500/410 m (tachyons ~2.6 km).
  Calling game code from C:
  call_far pushes the arguments and a far return, sets CS and runs it, then restores the registers.
- In H5 (and probably any loaded save) every gun starts off (+35h = 0): the guns fire only after a gun
  key (1-4 or G). Scripted fire tests need that first.
- The web build doesn't do Privateer yet: no shared mappings there (aliased EMS pages), and the
  page knows WC.EXE and SM2.EXE only.

## Two players (runtime/net.c; Wing Commander 1 only)

- Without `WC_NET` none of this may change the game: whatever the runtime calls in net.c returns at
  once on `wc_net_on` (`net_frame` didn't, and hung at the first flight frame). Checked by running the
  same `WC_KEYS` on a build of the previous commit: memory, picture and files alike but for the clock
  value the game stores at startup (143a4h and 1c7c0h, which also differ between two runs of one build).
  The web build compiles net.c too (no `arc4random` there).
- Lockstep over UDP: both PCs run the whole game on the instruction clock, paced to real time
  (`net_pace`); inputs wait for blocks of 1/30 s of emulated time and are applied at the same
  instruction on both. `pc_mouse`, `pc_joystick` and `key_out` hand local input to net.c.
- Anything from the host's world breaks it: speed keys are off, DOS's and the BIOS's time and file
  dates come from `net_time`, and the handshake compares WC.EXE, WINGCMDR.CFG, SAVEGAME.WLD and the
  environment that matters. Machine hashes (first 640 KB + registers) go over the wire once a second.
- Objects are parallel arrays by DS offset; 0 is the player, 1 the wingman (DS:662c), state
  `bd1a` 12 means normal flight. Slots: positions `a9c2`, orientation `aeb6`/`b1b6`/`b4b6`, type
  `bff8`, side `be94`, set speed `b9f6` (long, HUD kps * 25.6), top speed `ae60` (* 256), guns'
  energy `be3a`, shields `c988`, armor `c85e`. `swap_builtin` lists the arrays, found by pattern in
  the decompiled code, so some may be wrong or missing.
- AI off: at ovl09_28d0's entry (hook) the wingman's state is set to one its switch ignores and put
  back at ovl11_0606's entry, where the stick goes in as target turning speeds. Guns: ovl11_2e76(ship).
  Nav points check ship 0 only (FUN_13be_1ff3(0)), so the friend can't trigger them.
- The friend's cockpit is a `fork()` per flight frame on their side: the child swaps objects 0 and
  1, runs two frames, publishes the picture in shared memory and exits, so it can't touch the game.
  It must not reach host code holding locks (`vga_poll`, sound, `poll_keyboard` return early) and
  leaves at other DOS file calls. No `fork()` in the browser: there, copy and restore (`WC_SWAP_MODE=fork`).
  A child that fails ends silently, and the window keeps the last picture; `WC_VIEW_LOG=1` reports
  it. Swapping arrays at odd offsets as 4-byte elements (the pattern finds `bb01`, the array is word
  `bb02`) scrambled the draw parameters: it only showed with ship 3 on screen, as "missing code"
  in the child. Test with `WC_FRIEND_CHASE=n` on both sides: the friend's stick chases ship n.
- The friend's keys step the stick like the game's own (FUN_13be_0945): one step a frame while a key is
  down, the other way stops first (`friend_steer`, from NET_KEY_* bits in the blocks; both PCs and the view
  run it). The friend's stick stops at 8 (~7.8 degrees a frame); the player's goes 8, 9, 8, 9 (~8.35, unevenly). `friend_hook` writes both the current rates and the wanted ones: ovl11_0606 moves the first to the second at a limited rate, which held a turn back, on a diagonal to half. Tab goes in
  the block (`friend_burner`, ovl11_35d8's logic without its random damage check: 1235 kps checked). T and L stay on
  the friend's side: `view_next_key`
  feeds them to the game's key code in one child through 6609, and the target (c284), lock (6641) and
  right display pass to the next child through the shared memory.
- Targeted in the friend's view, the commander is in the wingman's place and would read SPIRIT: each child points the pilot's roster entry (98ca) at the player's, the roster's last, unless a radio message is up, which needs the pilot's. Enemies aren't removed for being far from the friend: they stay with the commander, out of the friend's radar and sight.
- System damage (d24c, 9 bytes by system, ovl02_0dec) is the player's alone: only ship 0 takes it (ovl11_0edb's `param_2 == 0`), a wingman's hits go to shields and armor. The real game was never affected, but the friend's cockpit was a copy of the commander's: each child clears the list and the texts ("%s HIT"...), and draws the friend's own displays (`view_displays`: weapons, and navigation or the target; a radio message's face stays). So what the commander chooses or has damaged (`WC_COMMANDER_HURT=frame:mask of systems` on both sides, for testing; 8, the computer, blanks the displays) stays theirs.
- The friend's mouse: `mouse_event` in net.c makes it a stick (`pc_mouse_stick`, the steps of FUN_13be_0d0d's tables at d1 and dd; a steering key sets it aside until it moves again) and buttons: left the guns, both a missile, right tapped and held within 0.5 s the afterburner (the game's window, 84c0 = 20 of its timer, wasn't worked out). Each child draws their pointer (00c6, 84ee, 84f0) from the last state before its fork.
- The friend's missiles: Enter in the blocks (NET_KEY_MISSILE), ovl11_2dce(1) on both PCs, which fires the first in the wingman's list in c472 (51 bytes a ship, swapped in the view). Their target (c284) only their view knows, so the blocks carry it: the game's c284[1] is set from them when firing. Not predicted: it shows when the input is due.
- The friend's view runs ahead while they fly (`view_rates`, `net_ahead`; `WC_NET_PREDICT=0` off), always
  the same number of frames (`view_lead`, 3 at delay 3 and the default 15 fps, ~5 ms a picture): a number
  that varied made the picture step 0 or 2 frames at a time. It must match the wingman frame for frame or the
  picture jerks at every turn: a frame goes by the block whose number its slot gives (`net_frame`; blocks and
  frames meet within a millisecond and interrupts decide on which side, so never by what is in force), the wingman's stick turns it the frame after and the player's the same frame, so the
  child's stick is the one from a frame before, and the keys of the two blocks to a frame are or-ed. Compare
  what is on the screen, the frame before the last (`WC_VIEW_PRED=1`), with the game's. Key to picture ~30-70
  ms against ~160 ms without; only the commander's next move can be wrong, and that ends with the child. A stall on
  the other side's inputs still stops the pictures: nothing is forked while the game waits. It may read
  files it opens itself (`view_file_call`; the game opens DISK.004 for read and write every 32 frames
  in one test flight, only there): a file opened before the fork shares its position with the game.
  `WC_VIEW_PRED=1` logs keys, pictures and ships by frame, to compare with the game's.
- `Origin l s1 m0` launches a mission directly (`l`), handy for tests; `-k` is invulnerability.

## Conventions

- TypeScript run directly by Node (≥ 23.6), ESM, `npm run typecheck`. Java Ghidra scripts in
  `ghidra_scripts/`. C11 runtime in `runtime/`, tab-indented, short comments.
- Debug env vars (`WC_KEYS` key names: esc, enter, space, tab, backspace, arrows, f1-f10, alt, ctrl, characters): `WC_TRACE` (1 DOS/IRQ, 2 every instruction, 3 more), `WC_KEYS`, `WC_LOAD_SEG`,
  `WC_POKE`, `WC_WATCH`, `WC_SCREENSHOT`, `WC_LOADED_DIR`, `WC_HEADLESS`, `WC_CYCLES`, `WC_MOUSE`, `WC_WAV`, `WC_EMS`, `WC_WARP`, `WC_JOYSTICK`, `WC_STICK_KEYS` (stick buttons to keys, pc.c), `WC_ROLL_AXIS`, `WC_MT32`, `WC_MT32_MACHINE`, `WC_SB`, `WC_DUMP` (1 MB memory file at a `quit` key or the first
  write into driver code; watch hits print the call stack), `WC_TIME` (DOS's clock starts at these
  seconds since 1970 and runs on emulated time: with the instruction clock, Privateer's runs repeat
  exactly, so memory dumps of two runs can be compared), `WC_CALLS=from:to[:addr]` (counts the indirect
  calls, virtual methods included, by target between those ticks, in the report; with addr the callers
  of that target: compare two runs to find the code an action runs). Ctrl-C prints a report.
