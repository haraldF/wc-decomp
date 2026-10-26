// Parser for Borland's VROOM overlay format (Turbo C++ / Borland C++ "FBOV" overlays).
//
// Layout, as found in WC.EXE:
//  - After the MZ load image comes a 16-byte header: "FBOV", u32 overlay data size,
//    u32 file offset of the segment table, u32 segment table entry count.
//  - The segment table has 8-byte entries: u16 segment, u16 size, u16 flags, u16 min offset.
//    Flags: 0 = data, 1 = code, 3 = overlay stub, 4 = other (BSS/stack/far data).
//  - Each stub segment starts with a 32-byte header:
//      +0 "CD 3F" (int 3Fh), +4 u32 offset of the overlay's code in the overlay data,
//      +8 u16 code size, +10 u16 relocation table size in bytes, +12 u16 entry count.
//    followed by one 5-byte entry per exported function: "CD 3F", u16 offset in overlay code, 0.
//  - The overlay data holds each overlay's code followed by its relocations: u16 offsets of
//    segment words inside the code. Unlike MZ relocations, each such word holds a byte offset
//    into the segment table (index * 8); the overlay manager replaces it with that segment.

import type { MzFile } from "./mz.ts";

export type SegmentKind = "data" | "code" | "stub" | "other";

export interface SegmentInfo {
  seg: number;
  size: number;
  minOff: number;
  flags: number;
  kind: SegmentKind;
}

export interface Overlay {
  index: number;
  stubSeg: number;
  code: Buffer;
  /** Offsets within `code` of words holding segment values. */
  relocs: number[];
  /** Offsets within `code` of the overlay's entry points, one per stub entry. */
  entries: number[];
}

export interface VroomInfo {
  segments: SegmentInfo[];
  overlays: Overlay[];
}

const KINDS: Record<number, SegmentKind> = { 0: "data", 1: "code", 3: "stub", 4: "other" };
export const STUB_HEADER_SIZE = 0x20;
export const STUB_ENTRY_SIZE = 5;

export function parseVroom(file: Buffer, mz: MzFile): VroomInfo | undefined {
  const hdr = mz.imageEnd;
  if (file.length < hdr + 16 || file.toString("latin1", hdr, hdr + 4) !== "FBOV") return undefined;
  const dataSize = file.readUInt32LE(hdr + 4);
  const tableOffset = file.readUInt32LE(hdr + 8);
  const tableCount = file.readUInt32LE(hdr + 12);
  const data = file.subarray(hdr + 16, hdr + 16 + dataSize);

  const segments: SegmentInfo[] = [];
  for (let i = 0; i < tableCount; i++) {
    const p = tableOffset + i * 8;
    const flags = file.readUInt16LE(p + 4);
    segments.push({
      seg: file.readUInt16LE(p),
      size: file.readUInt16LE(p + 2),
      flags,
      minOff: file.readUInt16LE(p + 6),
      kind: KINDS[flags] ?? "other",
    });
  }

  const overlays = segments
    .filter((s) => s.kind === "stub")
    .map((s, index) => parseStub(mz.image, data, s.seg, index));
  return { segments, overlays };
}

function parseStub(image: Buffer, data: Buffer, stubSeg: number, index: number): Overlay {
  const p = stubSeg * 16;
  if (image.readUInt16LE(p) !== 0x3fcd) throw new Error(`stub ${hex(stubSeg)} has no int 3Fh header`);
  const codeOffset = image.readUInt32LE(p + 4);
  const codeSize = image.readUInt16LE(p + 8);
  const relocBytes = image.readUInt16LE(p + 10);
  const entryCount = image.readUInt16LE(p + 12);

  const entries: number[] = [];
  for (let i = 0; i < entryCount; i++) {
    const e = p + STUB_HEADER_SIZE + i * STUB_ENTRY_SIZE;
    if (image.readUInt16LE(e) !== 0x3fcd) throw new Error(`stub ${hex(stubSeg)} entry ${i} is malformed`);
    entries.push(image.readUInt16LE(e + 2));
  }

  const code = data.subarray(codeOffset, codeOffset + codeSize);
  const relocs: number[] = [];
  for (let i = 0; i < relocBytes; i += 2) {
    const r = data.readUInt16LE(codeOffset + codeSize + i);
    if (r + 2 > codeSize) throw new Error(`overlay ${index} relocation ${hex(r)} is outside its code`);
    relocs.push(r);
  }
  return { index, stubSeg, code, relocs, entries };
}

export function hex(n: number, width = 4): string {
  return n.toString(16).padStart(width, "0");
}
