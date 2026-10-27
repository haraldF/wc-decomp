// Writes a save game that starts at any mission of the campaign tree, into a free slot of the
// game's GAMEDAT/SAVEGAME.WLD, or CRUSADE.WLD for the Secret Missions 2 (SM2.EXE) (load it in the
// barracks: "Continue Campaign").
//
// Usage: node src/mission.ts [--game dir] --list                 show the campaigns' mission trees
//        node src/mission.ts [--game dir] [options] <mission>    write the save game
//   <mission>        [campaign:]series.mission, e.g. 4.2, gimle.2, sm1:3.1: a series by number or
//                    (the start of) its system's name, missions from 1; campaigns: vega, sm1 (Secret Missions),
//                    sm2 (Secret Missions 2)
//   --slot n         slot to write (1-8); needed when no slot is free, and then it is overwritten
//   --title text     the save game's name (default: system and mission)
//   --name, --callsign  the pilot (default: the game's own)
//   --move           with --slot: move that save game to the mission instead, keeping its pilot
//                    (kills, rank, medals, wingmen), name and date
//   --transfer n     start the mission with the pilot (and name) of SAVEGAME.WLD's slot n, e.g. a
//                    Wing Commander pilot in the Secret Missions 2 (sm2:1.1), like the game's
//                    TRANSFER.EXE/TRANS2.EXE, which only take pilots from Venice or the Secret Missions
//
// The save holds the state of a new campaign, taken from the game's WC.EXE or SM2.EXE (as the game copies it when
// a campaign starts), with the series and mission set and a series history along a path through
// the tree to them (preferring won series). Scores, kills, medals and rank are those of a new pilot
// (or, with --move or --transfer, the save game's).
// The save file is copied to SAVEGAME.BAK (CRUSADE.BAK) before it is changed.

import { copyFileSync, existsSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { parseArgs } from "node:util";
import { parseMz } from "./mz.ts";

const root = path.resolve(import.meta.dirname, "..");
const { positionals, values } = parseArgs({
  allowPositionals: true,
  options: {
    game: { type: "string", default: path.resolve(root, "../game") },
    list: { type: "boolean", default: false },
    slot: { type: "string" },
    title: { type: "string" },
    name: { type: "string" },
    callsign: { type: "string" },
    move: { type: "boolean", default: false },
    transfer: { type: "string" },
  },
});

// SAVEGAME.WLD: 8 slots of SLOT_SIZE bytes. A slot is its name (17 bytes), a "used" byte, then the
// game's pilot roster, campaign state and last mission's flight records as they are in memory.
const SLOTS = 8;
const SLOT_SIZE = 0x33c;
const USED = 0x11;
const ROSTER = 0x12;        // 9 pilots of 38 bytes (name 14, callsign 14, ...), the player last (DS:9912)
const ROSTER_SIZE = 342;
const PILOT_SIZE = 38;
const PLAYER = 8;
const STATE = ROSTER + ROSTER_SIZE; // campaign state (DS:c240)
const STATE_SIZE = 0x44;
// Campaign state fields (offsets in STATE).
const WAR_STATUS = 0x02;    // set by the series transition taken (series record +7/+9)
const MISSION = 0x15;       // from 0
const SERIES = 0x16;        // from 1
const HISTORY_COUNT = 0x17; // series completed so far
const HISTORY = 0x18;       // ... and which (8 bytes)
const DAY = 0x34, YEAR = 0x36;
const SERIES_SCORE = 0x40;  // word, from 0 in each series
const CAMPAIGN = 0x42;      // 0 Vega, 1 the Secret Missions, 2 the Secret Missions 2 (CAMP.00n)

// The programs, their save files, and where their data segment keeps the new-campaign state the
// game copies (ovl17_10c9 in WC.EXE).
interface Program { exe: string; saves: string; backup: string; dgroup: number; newState: number; newRoster: number }
const WC: Program = { exe: "WC.EXE", saves: "SAVEGAME.WLD", backup: "SAVEGAME.BAK", dgroup: 0x12310, newState: 0x17c8, newRoster: 0x180c };
const SM2: Program = { exe: "SM2.EXE", saves: "CRUSADE.WLD", backup: "CRUSADE.BAK", dgroup: 0x11e80, newState: 0x171c, newRoster: 0x1760 };

// WC.EXE runs the first two (its campaign file table has no third); SM2.EXE the Secret Missions 2
// (its table, DS:1f00, lists all three files, and it starts the third).
const CAMPAIGNS = ["vega", "sm1", "sm2"];
const programOf = (campaign: number): Program => (campaign === 2 ? SM2 : WC);

/** Origin's LZW: LSB-first codes of 9 to 12 bits, 100h resets the dictionary, 101h ends. */
function lzw(src: Buffer, size: number): Buffer {
  const out = Buffer.alloc(size);
  let n = 0, pos = 0, bits = 9, next = 0x102;
  let dict: Buffer[] = [];
  let prev: Buffer | null = null;
  const reset = (): void => {
    dict = Array.from({ length: 256 }, (_, i) => Buffer.from([i]));
    next = 0x102;
    bits = 9;
    prev = null;
  };
  reset();
  while (n < size) {
    const i = pos >> 3;
    const word = (src[i] ?? 0) | ((src[i + 1] ?? 0) << 8) | ((src[i + 2] ?? 0) << 16);
    const code = (word >> (pos & 7)) & ((1 << bits) - 1);
    pos += bits;
    if (code === 0x100) {
      reset();
      continue;
    }
    if (code === 0x101) break;
    let entry: Buffer;
    if (prev === null) {
      entry = dict[code];
    } else {
      entry = code < next ? dict[code] : Buffer.concat([prev, prev.subarray(0, 1)]);
      dict[next++] = Buffer.concat([prev, entry.subarray(0, 1)]);
      if (next >= 1 << bits && bits < 12) bits++;
    }
    if (!entry) throw new Error(`bad LZW code ${code}`);
    n += entry.copy(out, n);
    prev = entry;
  }
  return out;
}

/** The entries of an Origin resource file (a size, then 24-bit offsets with a flags byte; 1: compressed). */
function resources(file: Buffer): Buffer[] {
  const count = (file.readUInt32LE(4) & 0xffffff) / 4 - 1;
  return Array.from({ length: count }, (_, i) => {
    const e = file.readUInt32LE(4 + i * 4);
    const off = e & 0xffffff;
    const size = file.readUInt32LE(off);
    return e >>> 24 ? lzw(file.subarray(off + 4), size) : file.subarray(off + 4, off + 4 + size);
  });
}

interface Series {
  number: number;
  name: string;         // its star system
  missions: number;
  win: number;          // next series (0xff: the campaign ends)
  winStatus: number;
  lose: number;
  loseStatus: number;
  end: number;          // 40h victory, 41h defeat
}

/** The series records (90 bytes each) of CAMP.00n's second entry, named from MODULE.00n's sixth
 * (40-byte names, by series number; 0 is "Squadron"). */
function readCampaign(campaign: number): Series[] {
  const file = path.join(values.game, "GAMEDAT", `CAMP.00${campaign}`);
  if (!existsSync(file)) return [];
  const table = resources(readFileSync(file))[1];
  const names = resources(readFileSync(path.join(values.game, "GAMEDAT", `MODULE.00${campaign}`)))[5];
  const series: Series[] = [];
  for (let i = 0; i * 90 < table.length; i++) {
    const r = table.subarray(i * 90, i * 90 + 90);
    if (r[2] === 0) continue;
    const name = names.toString("latin1", (i + 1) * 40, (i + 2) * 40).replace(/\0.*$/s, "").replace(/^-/, "");
    series.push({ number: i + 1, name, missions: r[2], end: r[5], win: r[6], winStatus: r[7], lose: r[8], loseStatus: r[9] });
  }
  return series;
}

const seriesName = (series: Series[], n: number): string => `${n} ${series.find((s) => s.number === n)?.name ?? "?"}`;

function listCampaigns(): void {
  CAMPAIGNS.forEach((name, c) => {
    const series = readCampaign(c);
    if (!series.length) return;
    console.log(`${name}:`);
    for (const s of series) {
      const next = s.win === 0xff
        ? (s.end === 0x40 ? "victory" : s.end === 0x41 ? "defeat" : "end")
        : `won: ${seriesName(series, s.win)}, lost: ${seriesName(series, s.lose)}`;
      console.log(`  ${seriesName(series, s.number).padEnd(17)} missions 1-${s.missions}   ${next}`);
    }
  });
}

/** A path from series 1 to `target` (the series flown, and the war status entering the last). */
function findPath(series: Series[], target: number): { history: number[]; status: number } | null {
  const byNumber = new Map(series.map((s) => [s.number, s]));
  const seen = new Set<number>();
  const visit = (n: number, history: number[], status: number): { history: number[]; status: number } | null => {
    if (n === target) return { history, status };
    const s = byNumber.get(n);
    if (!s || seen.has(n) || s.win === 0xff) return null;
    seen.add(n);
    return visit(s.win, [...history, n], s.winStatus) ?? visit(s.lose, [...history, n], s.loseStatus);
  };
  return visit(1, [], -1);
}

function parseMission(spec: string): { campaign: number; series: string; mission: number } {
  const m = /^(?:(\w+):)?(.+)\.(\d+)$/.exec(spec.trim());
  if (!m) throw new Error(`mission "${spec}": expected [campaign:]series.mission, e.g. 4.2 or gimle.2`);
  const campaign = m[1] ? CAMPAIGNS.indexOf(m[1].toLowerCase()) : 0;
  if (campaign < 0) throw new Error(`unknown campaign "${m[1]}" (${CAMPAIGNS.join(", ")})`);
  return { campaign, series: m[2], mission: Number(m[3]) };
}

/** A series by number or by (the start of) its system's name. */
function findSeries(series: Series[], key: string): Series {
  const simple = (s: string): string => s.toLowerCase().replace(/[^a-z0-9]/g, "");
  const found = /^\d+$/.test(key)
    ? series.filter((s) => s.number === Number(key))
    : series.filter((s) => simple(s.name).startsWith(simple(key)));
  if (found.length === 1) return found[0];
  if (!found.length) throw new Error(`no series "${key}" (see --list)`);
  throw new Error(`"${key}" is ambiguous: series ${found.map((s) => s.number).join(", ")}; give the number`);
}

/** The roster and campaign state the game sets up for a new campaign, from the program's data. */
function newCampaignState(p: Program): { roster: Buffer; state: Buffer } {
  const exe = parseMz(readFileSync(path.join(values.game, p.exe)));
  const roster = Buffer.from(exe.image.subarray(p.dgroup + p.newRoster, p.dgroup + p.newRoster + ROSTER_SIZE));
  const state = Buffer.from(exe.image.subarray(p.dgroup + p.newState, p.dgroup + p.newState + STATE_SIZE));
  if (roster.toString("latin1", 0, 6) !== "TANAKA" || state[SERIES] !== 1) {
    throw new Error(`${p.exe}: the new-campaign data isn't where expected (a different version of the game?)`);
  }
  return { roster, state };
}

function setString(buf: Buffer, at: number, size: number, text: string, what: string): void {
  if (text.length >= size) throw new Error(`${what} "${text}" is too long (at most ${size - 1} characters)`);
  buf.fill(0, at, at + size);
  buf.write(text, at, "latin1");
}

function writeSave(spec: string): void {
  const { campaign, series: key, mission } = parseMission(spec);
  const series = readCampaign(campaign);
  if (!series.length) throw new Error(`campaign ${CAMPAIGNS[campaign]}: no CAMP.00${campaign} in ${values.game}/GAMEDAT`);
  const s = findSeries(series, key);
  const target = s.number;
  if (mission < 1 || mission > s.missions) throw new Error(`series ${seriesName(series, target)} has missions 1-${s.missions}`);
  const route = findPath(series, target);
  if (!route) throw new Error(`series ${target} can't be reached from series 1`);

  const program = programOf(campaign);
  const file = path.join(values.game, "GAMEDAT", program.saves);
  const saves = existsSync(file) ? readFileSync(file) : Buffer.alloc(0);
  const wld = Buffer.alloc(SLOTS * SLOT_SIZE);
  if (saves.length && saves.length !== wld.length) throw new Error(`${file}: ${saves.length} bytes, expected ${wld.length}`);
  saves.copy(wld);
  const used = (i: number): boolean => wld[i * SLOT_SIZE + USED] !== 0;
  let slot: number;
  if (values.slot !== undefined) {
    slot = Number(values.slot) - 1;
    if (!(slot >= 0 && slot < SLOTS)) throw new Error("--slot must be 1-8");
  } else {
    slot = Array.from({ length: SLOTS }, (_, i) => i).find((i) => !used(i)) ?? -1;
    if (slot < 0) {
      const names = Array.from({ length: SLOTS }, (_, i) => `  ${i + 1}: ${wld.toString("latin1", i * SLOT_SIZE, i * SLOT_SIZE + USED).replace(/\0.*$/s, "")}`);
      throw new Error(`no free save slot; choose one to overwrite with --slot n:\n${names.join("\n")}`);
    }
  }

  const overwritten = used(slot);
  const at = slot * SLOT_SIZE;
  if (values.move && (values.slot === undefined || !overwritten)) throw new Error("--move needs --slot n of a save game");
  if (values.move && values.transfer !== undefined) throw new Error("--move and --transfer don't go together");
  const { roster, state } = values.move
    ? { roster: Buffer.from(wld.subarray(at + ROSTER, at + ROSTER + ROSTER_SIZE)), state: Buffer.from(wld.subarray(at + STATE, at + STATE + STATE_SIZE)) }
    : newCampaignState(program);
  // --transfer: the pilots (the player, with kills, rank and medals, and the wingmen) of a Wing
  // Commander save game, in a new campaign
  let from = "";
  if (values.transfer !== undefined) {
    const src = Number(values.transfer) - 1;
    if (!(src >= 0 && src < SLOTS)) throw new Error("--transfer must be 1-8");
    const wcFile = path.join(values.game, "GAMEDAT", WC.saves);
    const wc = wcFile === file ? wld : existsSync(wcFile) ? readFileSync(wcFile) : Buffer.alloc(0);
    if (wc.length !== SLOTS * SLOT_SIZE || wc[src * SLOT_SIZE + USED] === 0) throw new Error(`${WC.saves} has no save game in slot ${src + 1}`);
    wc.copy(roster, 0, src * SLOT_SIZE + ROSTER, src * SLOT_SIZE + ROSTER + ROSTER_SIZE);
    from = wc.toString("latin1", src * SLOT_SIZE, src * SLOT_SIZE + USED).replace(/\0.*$/s, "");
  }
  const player = PLAYER * PILOT_SIZE;
  if (values.name !== undefined) setString(roster, player, 14, values.name, "name");
  if (values.callsign !== undefined) setString(roster, player + 14, 14, values.callsign, "callsign");
  state[CAMPAIGN] = campaign;
  state[SERIES] = target;
  state[MISSION] = mission - 1;
  if (route.status >= 0) state[WAR_STATUS] = route.status;
  state.fill(0, HISTORY, HISTORY + 8);
  state[HISTORY_COUNT] = route.history.length;
  route.history.forEach((n, i) => (state[HISTORY + i] = n));
  state.writeUInt16LE(0, SERIES_SCORE);
  if (!values.move) {
    // The game moves the date on by about a day per mission, five more for a new series.
    const flown = route.history.reduce((d, n) => d + series.find((x) => x.number === n)!.missions, 0) + mission - 1;
    let day = state.readUInt16LE(DAY) + flown + 5 * route.history.length;
    let year = state.readUInt16LE(YEAR);
    for (; day > 365; day -= 365) year++;
    state.writeUInt16LE(day, DAY);
    state.writeUInt16LE(year, YEAR);
    wld.fill(0, at, at + SLOT_SIZE);
  }
  const title = values.title ?? (values.move ? wld.toString("latin1", at, at + USED).replace(/\0.*$/s, "")
    : from || `${campaign ? CAMPAIGNS[campaign].toUpperCase() + " " : ""}${s.name} ${mission}`);
  setString(wld, at, USED, title.slice(0, USED - 1), "title");
  wld[at + USED] = 1;
  roster.copy(wld, at + ROSTER);
  state.copy(wld, at + STATE);

  if (existsSync(file)) copyFileSync(file, path.join(values.game, "GAMEDAT", program.backup));
  writeFileSync(file, wld);
  const via = route.history.map((n) => seriesName(series, n)).join(" > ");
  console.log(`${program.saves} slot ${slot + 1}${values.move ? " (moved)" : overwritten ? " (overwritten)" : ""}: "${title.slice(0, USED - 1)}", ` +
    (from ? `pilot from ${WC.saves} slot ${Number(values.transfer)}, ` : "") +
    `${CAMPAIGNS[campaign]} series ${seriesName(series, target)}, mission ${mission}${via ? ` (after ${via})` : ""}`);
}

try {
  if (values.list) listCampaigns();
  else if (positionals.length === 1) writeSave(positionals[0]);
  else throw new Error("usage: node src/mission.ts [--game dir] (--list | [--slot n [--move]] [--transfer n] [--title t] [--name n] [--callsign c] <[campaign:]series.mission>)");
} catch (e) {
  console.error(`mission: ${(e as Error).message}`);
  process.exit(1);
}
