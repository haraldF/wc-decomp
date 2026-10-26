// Minimal reader/writer for DOS MZ executables.

export interface MzHeader {
  lastPageBytes: number;
  pages: number;
  relocCount: number;
  headerParas: number;
  minAlloc: number;
  maxAlloc: number;
  ss: number;
  sp: number;
  checksum: number;
  ip: number;
  cs: number;
  relocOffset: number;
  overlayNumber: number;
}

/** A far pointer (segment:offset, segment relative to the load base) to a word that holds a segment value. */
export interface Reloc {
  seg: number;
  off: number;
}

export interface MzFile {
  header: MzHeader;
  relocs: Reloc[];
  /** The load image: everything the DOS loader copies into memory. */
  image: Buffer;
  /** File offset where the load image ends (start of any appended overlay data). */
  imageEnd: number;
}

const HEADER_FIELDS = [
  "lastPageBytes", "pages", "relocCount", "headerParas", "minAlloc", "maxAlloc",
  "ss", "sp", "checksum", "ip", "cs", "relocOffset", "overlayNumber",
] as const;

export function parseMz(file: Buffer): MzFile {
  if (file.toString("latin1", 0, 2) !== "MZ") throw new Error("not an MZ executable");
  const header = {} as MzHeader;
  HEADER_FIELDS.forEach((name, i) => (header[name] = file.readUInt16LE(2 + i * 2)));

  const relocs: Reloc[] = [];
  for (let i = 0; i < header.relocCount; i++) {
    const p = header.relocOffset + i * 4;
    relocs.push({ off: file.readUInt16LE(p), seg: file.readUInt16LE(p + 2) });
  }

  const fileSize = (header.pages - 1) * 512 + (header.lastPageBytes || 512);
  const imageStart = header.headerParas * 16;
  return { header, relocs, image: file.subarray(imageStart, fileSize), imageEnd: fileSize };
}

/** Build an MZ file from an image and relocation table; header fields not given are copied from `base`. */
export function buildMz(base: MzHeader, image: Buffer, relocs: Reloc[]): Buffer {
  const relocOffset = 0x1c;
  const headerBytes = align(relocOffset + relocs.length * 4, 512);
  if (relocs.length > 0xffff) throw new Error(`too many relocations: ${relocs.length}`);

  const out = Buffer.alloc(headerBytes + image.length);
  const total = out.length;
  const header: MzHeader = {
    ...base,
    lastPageBytes: total % 512,
    pages: Math.ceil(total / 512),
    relocCount: relocs.length,
    headerParas: headerBytes / 16,
    relocOffset,
    overlayNumber: 0,
    checksum: 0,
  };
  out.write("MZ", 0, "latin1");
  HEADER_FIELDS.forEach((name, i) => out.writeUInt16LE(header[name], 2 + i * 2));
  relocs.forEach((r, i) => {
    out.writeUInt16LE(r.off, relocOffset + i * 4);
    out.writeUInt16LE(r.seg, relocOffset + i * 4 + 2);
  });
  image.copy(out, headerBytes);
  return out;
}

export function align(n: number, to: number): number {
  return Math.ceil(n / to) * to;
}
