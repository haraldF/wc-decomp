// Turns an overlaid Borland EXE into a plain MZ EXE that Ghidra can load in one piece:
// every overlay's code is appended to the load image as a new segment, its relocations are
// added to the MZ relocation table, and each stub entry ("int 3Fh" trampoline) is rewritten
// into a direct "jmp far" to the function inside the now-resident overlay segment.
// The result is meant for static analysis only; it is not expected to run under DOS.

import { align, buildMz, parseMz, type Reloc } from "./mz.ts";
import { hex, parseVroom, STUB_ENTRY_SIZE, STUB_HEADER_SIZE, type SegmentKind } from "./vroom.ts";

export interface ManifestSegment {
  seg: number;
  size: number;
  minOff: number;
  kind: SegmentKind | "overlay";
  name: string;
}

export interface ManifestOverlay {
  index: number;
  stubSeg: number;
  seg: number;
  size: number;
  relocCount: number;
  /** stubOffset: entry offset inside the stub segment (what the original code far-calls). */
  entries: { stubOffset: number; target: number }[];
}

export interface Manifest {
  source: string;
  entry: { cs: number; ip: number };
  /** Initial stack, from the EXE header. */
  stack: { ss: number; sp: number };
  /** Segment of DGROUP (the default data segment Borland's startup code loads into DS). */
  dgroup: number | null;
  segments: ManifestSegment[];
  overlays: ManifestOverlay[];
}

export function flatten(file: Buffer, source: string): { exe: Buffer; manifest: Manifest } {
  const mz = parseMz(file);
  const vroom = parseVroom(file, mz);
  if (!vroom) throw new Error("no FBOV overlay data found after the load image");

  const relocs: Reloc[] = [...mz.relocs];
  let imageSize = align(mz.image.length, 16);
  const placed = vroom.overlays.map((ovl) => {
    const seg = imageSize / 16;
    imageSize = align(imageSize + ovl.code.length, 16);
    return { ovl, seg };
  });
  if (imageSize > 0xa0000) throw new Error("flattened image does not fit in conventional memory");

  const image = Buffer.alloc(imageSize);
  mz.image.copy(image);
  for (const { ovl, seg } of placed) {
    ovl.code.copy(image, seg * 16);
    for (const off of ovl.relocs) {
      const p = seg * 16 + off;
      const tableOffset = image.readUInt16LE(p);
      const target = vroom.segments[tableOffset / 8];
      if (tableOffset % 8 !== 0 || !target) {
        throw new Error(`overlay ${ovl.index} relocation at ${hex(off)} has bad segment table offset ${hex(tableOffset)}`);
      }
      image.writeUInt16LE(target.seg, p);
      relocs.push({ seg, off });
    }
    ovl.entries.forEach((target, i) => {
      const off = STUB_HEADER_SIZE + i * STUB_ENTRY_SIZE;
      const p = ovl.stubSeg * 16 + off;
      image[p] = 0xea; // jmp far seg:target
      image.writeUInt16LE(target, p + 1);
      image.writeUInt16LE(seg, p + 3);
      relocs.push({ seg: ovl.stubSeg, off: off + 3 });
    });
  }

  const entryCode = mz.header.cs * 16 + mz.header.ip;
  const dgroup = image[entryCode] === 0xba ? image.readUInt16LE(entryCode + 1) : null; // mov dx, DGROUP

  const segments: ManifestSegment[] = vroom.segments.map((s) => ({
    seg: s.seg,
    size: s.size,
    minOff: s.minOff,
    kind: s.kind,
    name: `${s.kind}_${hex(s.seg)}`,
  }));
  for (const { ovl, seg } of placed) {
    segments.push({ seg, size: ovl.code.length, minOff: 0, kind: "overlay", name: `ovl${String(ovl.index).padStart(2, "0")}` });
  }

  const manifest: Manifest = {
    source,
    entry: { cs: mz.header.cs, ip: mz.header.ip },
    stack: { ss: mz.header.ss, sp: mz.header.sp },
    dgroup,
    segments,
    overlays: placed.map(({ ovl, seg }) => ({
      index: ovl.index,
      stubSeg: ovl.stubSeg,
      seg,
      size: ovl.code.length,
      relocCount: ovl.relocs.length,
      entries: ovl.entries.map((target, i) => ({ stubOffset: STUB_HEADER_SIZE + i * STUB_ENTRY_SIZE, target })),
    })),
  };

  return { exe: buildMz(mz.header, image, relocs), manifest };
}

