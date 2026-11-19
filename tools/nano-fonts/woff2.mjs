/**
 * Unpacks a WOFF2 font (the web UI's theme fonts are only kept as WOFF2) back into the TrueType
 * file it was made from, so the native UI's font tools and DirectWrite can read it. Follows the
 * W3C WOFF2 specification: the Brotli stream (node:zlib), the known-tag table directory, and the
 * glyf/loca and hmtx transforms. Single fonts only, not collections.
 *
 *   import { decodeWoff2 } from './woff2.mjs';
 *   const ttf = decodeWoff2(fs.readFileSync('Font.woff2'));
 */

import zlib from 'node:zlib';

const KNOWN_TAGS = [
  'cmap', 'head', 'hhea', 'hmtx', 'maxp', 'name', 'OS/2', 'post', 'cvt ', 'fpgm', 'glyf', 'loca', 'prep',
  'CFF ', 'VORG', 'EBDT', 'EBLC', 'gasp', 'hdmx', 'kern', 'LTSH', 'PCLT', 'VDMX', 'vhea', 'vmtx', 'BASE',
  'GDEF', 'GPOS', 'GSUB', 'EBSC', 'JSTF', 'MATH', 'CBDT', 'CBLC', 'COLR', 'CPAL', 'SVG ', 'sbix', 'acnt',
  'avar', 'bdat', 'bloc', 'bsln', 'cvar', 'fdsc', 'feat', 'fmtx', 'fvar', 'gvar', 'hsty', 'just', 'lcar',
  'mort', 'morx', 'opbd', 'prop', 'trak', 'Zapf', 'Silf', 'Glat', 'Gloc', 'Feat', 'Sill',
];

// Composite glyph flags.
const ARG_1_AND_2_ARE_WORDS = 0x0001;
const WE_HAVE_A_SCALE = 0x0008;
const MORE_COMPONENTS = 0x0020;
const WE_HAVE_AN_X_AND_Y_SCALE = 0x0040;
const WE_HAVE_A_TWO_BY_TWO = 0x0080;
const WE_HAVE_INSTRUCTIONS = 0x0100;

// Simple glyph point flags.
const ON_CURVE = 0x01;
const X_SHORT = 0x02;
const Y_SHORT = 0x04;
const X_SAME_OR_POSITIVE = 0x10;
const Y_SAME_OR_POSITIVE = 0x20;
const OVERLAP_SIMPLE = 0x40;

function fail(message) {
  throw new Error(`woff2: ${message}`);
}

/** A cursor over a buffer, big-endian, failing on overrun. */
class Reader {
  constructor(buffer, offset = 0, end = buffer.length) {
    this.buffer = buffer;
    this.offset = offset;
    this.end = end;
  }

  need(count) {
    if (this.offset + count > this.end)
      fail('data runs past the end of its stream');
  }

  u8() {
    this.need(1);
    return this.buffer[this.offset++];
  }

  u16() {
    this.need(2);
    const value = this.buffer.readUInt16BE(this.offset);
    this.offset += 2;
    return value;
  }

  i16() {
    this.need(2);
    const value = this.buffer.readInt16BE(this.offset);
    this.offset += 2;
    return value;
  }

  u32() {
    this.need(4);
    const value = this.buffer.readUInt32BE(this.offset);
    this.offset += 4;
    return value;
  }

  bytes(count) {
    this.need(count);
    const slice = this.buffer.subarray(this.offset, this.offset + count);
    this.offset += count;
    return slice;
  }

  base128() {
    let value = 0;

    for (let i = 0; i < 5; ++i) {
      const byte = this.u8();

      if (i === 0 && byte === 0x80)
        fail('UIntBase128 with a leading zero');

      if (value & 0xfe000000)
        fail('UIntBase128 overflows 32 bits');

      value = (value << 7) | (byte & 0x7f);

      if ((byte & 0x80) === 0)
        return value >>> 0;
    }

    fail('UIntBase128 longer than 5 bytes');
  }

  u255() {
    const code = this.u8();

    if (code === 253)
      return this.u16();
    if (code === 255)
      return this.u8() + 253;
    if (code === 254)
      return this.u8() + 253 * 2;
    return code;
  }
}

/** A growable big-endian byte writer. */
class Writer {
  constructor() {
    this.chunks = [];
    this.length = 0;
  }

  push(buffer) {
    this.chunks.push(buffer);
    this.length += buffer.length;
  }

  u8(value) {
    this.push(Buffer.of(value & 0xff));
  }

  u16(value) {
    const b = Buffer.alloc(2);
    b.writeUInt16BE(value & 0xffff);
    this.push(b);
  }

  i16(value) {
    const b = Buffer.alloc(2);
    b.writeInt16BE(value);
    this.push(b);
  }

  pad4() {
    const extra = (4 - (this.length % 4)) % 4;
    if (extra)
      this.push(Buffer.alloc(extra));
  }

  toBuffer() {
    return Buffer.concat(this.chunks, this.length);
  }
}

function withSign(flag, base) {
  return flag & 1 ? base : -base;
}

/** Decodes one point's x and y deltas from its triplet flag and the glyph stream. */
function readTriplet(flag, glyphs) {
  if (flag < 10)
    return [0, withSign(flag, ((flag & 14) << 7) + glyphs.u8())];

  if (flag < 20)
    return [withSign(flag, (((flag - 10) & 14) << 7) + glyphs.u8()), 0];

  if (flag < 84) {
    const b0 = flag - 20;
    const b1 = glyphs.u8();
    return [withSign(flag, 1 + (b0 & 0x30) + (b1 >> 4)), withSign(flag >> 1, 1 + ((b0 & 0x0c) << 2) + (b1 & 0x0f))];
  }

  if (flag < 120) {
    const b0 = flag - 84;
    const dx = withSign(flag, 1 + (Math.floor(b0 / 12) << 8) + glyphs.u8());
    const dy = withSign(flag >> 1, 1 + (((b0 % 12) >> 2) << 8) + glyphs.u8());
    return [dx, dy];
  }

  if (flag < 124) {
    const b0 = glyphs.u8();
    const b1 = glyphs.u8();
    const b2 = glyphs.u8();
    return [withSign(flag, (b0 << 4) + (b1 >> 4)), withSign(flag >> 1, ((b1 & 0x0f) << 8) + b2)];
  }

  const b0 = glyphs.u8();
  const b1 = glyphs.u8();
  const b2 = glyphs.u8();
  const b3 = glyphs.u8();
  return [withSign(flag, (b0 << 8) + b1), withSign(flag >> 1, (b2 << 8) + b3)];
}

/** A simple glyph's points in TrueType's own encoding: flags, then x deltas, then y deltas. */
function encodePoints(points, overlap) {
  const flags = [];
  const xs = new Writer();
  const ys = new Writer();
  let lastX = 0;
  let lastY = 0;

  points.forEach((point, i) => {
    let flag = point.onCurve ? ON_CURVE : 0;

    if (i === 0 && overlap)
      flag |= OVERLAP_SIMPLE;

    const dx = point.x - lastX;
    const dy = point.y - lastY;

    if (dx === 0) {
      flag |= X_SAME_OR_POSITIVE;
    } else if (Math.abs(dx) < 256) {
      flag |= X_SHORT | (dx > 0 ? X_SAME_OR_POSITIVE : 0);
      xs.u8(Math.abs(dx));
    } else {
      xs.i16(dx);
    }

    if (dy === 0) {
      flag |= Y_SAME_OR_POSITIVE;
    } else if (Math.abs(dy) < 256) {
      flag |= Y_SHORT | (dy > 0 ? Y_SAME_OR_POSITIVE : 0);
      ys.u8(Math.abs(dy));
    } else {
      ys.i16(dy);
    }

    flags.push(flag);
    lastX = point.x;
    lastY = point.y;
  });

  return Buffer.concat([Buffer.from(flags), xs.toBuffer(), ys.toBuffer()]);
}

/** Rebuilds glyf and loca from the transformed glyf table (transform version 0). */
function reconstructGlyf(data, locaLength) {
  const header = new Reader(data);
  header.u16(); // reserved
  const optionFlags = header.u16();
  const numGlyphs = header.u16();
  const indexFormat = header.u16();
  const sizes = Array.from({ length: 7 }, () => header.u32());

  let offset = header.offset;
  const streams = sizes.map((size) => {
    const stream = new Reader(data, offset, offset + size);
    offset += size;
    if (offset > data.length)
      fail('glyf streams run past the table');
    return stream;
  });
  const [nContours, nPoints, flagStream, glyphStream, compositeStream, bboxStream, instructions] = streams;

  const overlapBitmap = optionFlags & 1 ? data.subarray(offset, offset + ((numGlyphs + 7) >> 3)) : null;
  const bboxBitmapLength = ((numGlyphs + 31) >> 5) << 2;
  const bboxBitmap = bboxStream.bytes(bboxBitmapLength);

  const glyf = new Writer();
  const offsets = [];
  const xMins = new Array(numGlyphs).fill(0);

  for (let glyph = 0; glyph < numGlyphs; ++glyph) {
    offsets.push(glyf.length);
    const contours = nContours.i16();
    const hasBbox = (bboxBitmap[glyph >> 3] & (0x80 >> (glyph & 7))) !== 0;

    if (contours === 0) {
      if (hasBbox)
        fail(`empty glyph ${glyph} has a bounding box`);
      continue;
    }

    if (contours < 0) {
      if (!hasBbox)
        fail(`composite glyph ${glyph} has no bounding box`);

      const bbox = [bboxStream.i16(), bboxStream.i16(), bboxStream.i16(), bboxStream.i16()];
      const start = compositeStream.offset;
      let haveInstructions = false;

      for (let more = true; more;) {
        const flags = compositeStream.u16();
        compositeStream.u16(); // glyph index
        let argBytes = flags & ARG_1_AND_2_ARE_WORDS ? 4 : 2;

        if (flags & WE_HAVE_A_SCALE)
          argBytes += 2;
        else if (flags & WE_HAVE_AN_X_AND_Y_SCALE)
          argBytes += 4;
        else if (flags & WE_HAVE_A_TWO_BY_TWO)
          argBytes += 8;

        compositeStream.bytes(argBytes);
        haveInstructions ||= (flags & WE_HAVE_INSTRUCTIONS) !== 0;
        more = (flags & MORE_COMPONENTS) !== 0;
      }

      glyf.i16(-1);
      bbox.forEach((v) => glyf.i16(v));
      glyf.push(data.subarray(start, compositeStream.offset));

      if (haveInstructions) {
        const length = glyphStream.u255();
        glyf.u16(length);
        glyf.push(instructions.bytes(length));
      }

      xMins[glyph] = bbox[0];
      glyf.pad4();
      continue;
    }

    const endPoints = [];
    let total = 0;

    for (let c = 0; c < contours; ++c) {
      total += nPoints.u255();
      endPoints.push(total - 1);
    }

    const points = [];
    let x = 0;
    let y = 0;

    for (let p = 0; p < total; ++p) {
      const flag = flagStream.u8();
      const [dx, dy] = readTriplet(flag & 0x7f, glyphStream);
      x += dx;
      y += dy;
      points.push({ x, y, onCurve: (flag & 0x80) === 0 });
    }

    const instructionLength = glyphStream.u255();
    let bbox;

    if (hasBbox) {
      bbox = [bboxStream.i16(), bboxStream.i16(), bboxStream.i16(), bboxStream.i16()];
    } else {
      bbox = [Infinity, Infinity, -Infinity, -Infinity];
      for (const point of points) {
        bbox[0] = Math.min(bbox[0], point.x);
        bbox[1] = Math.min(bbox[1], point.y);
        bbox[2] = Math.max(bbox[2], point.x);
        bbox[3] = Math.max(bbox[3], point.y);
      }
    }

    const overlap = overlapBitmap !== null && (overlapBitmap[glyph >> 3] & (0x80 >> (glyph & 7))) !== 0;

    glyf.i16(contours);
    bbox.forEach((v) => glyf.i16(v));
    endPoints.forEach((v) => glyf.u16(v));
    glyf.u16(instructionLength);
    glyf.push(instructions.bytes(instructionLength));
    glyf.push(encodePoints(points, overlap));
    xMins[glyph] = bbox[0];
    glyf.pad4();
  }

  offsets.push(glyf.length);

  const loca = Buffer.alloc(indexFormat ? (numGlyphs + 1) * 4 : (numGlyphs + 1) * 2);
  offsets.forEach((value, i) => {
    if (indexFormat)
      loca.writeUInt32BE(value, i * 4);
    else
      loca.writeUInt16BE(value / 2, i * 2);
  });

  if (loca.length !== locaLength)
    fail(`rebuilt loca is ${loca.length} bytes, the directory says ${locaLength}`);

  return { glyf: glyf.toBuffer(), loca, indexFormat, numGlyphs, xMins };
}

/** Rebuilds hmtx from its transformed form (version 1): left side bearings from glyph boxes. */
function reconstructHmtx(data, numHMetrics, numGlyphs, xMins) {
  const reader = new Reader(data);
  const flags = reader.u8();
  const advances = Array.from({ length: numHMetrics }, () => reader.u16());
  const lsbs = [];

  for (let i = 0; i < numHMetrics; ++i)
    lsbs.push(flags & 1 ? xMins[i] : reader.i16());
  for (let i = numHMetrics; i < numGlyphs; ++i)
    lsbs.push(flags & 2 ? xMins[i] : reader.i16());

  const out = new Writer();
  for (let i = 0; i < numGlyphs; ++i) {
    if (i < numHMetrics)
      out.u16(advances[i]);
    out.i16(lsbs[i]);
  }
  return out.toBuffer();
}

function checksum(buffer) {
  let sum = 0;
  const padded = buffer.length % 4 ? Buffer.concat([buffer, Buffer.alloc(4 - (buffer.length % 4))]) : buffer;
  for (let i = 0; i < padded.length; i += 4)
    sum = (sum + padded.readUInt32BE(i)) >>> 0;
  return sum;
}

/** The TrueType (or CFF) font inside `woff2`, as a Buffer. */
export function decodeWoff2(woff2) {
  const reader = new Reader(woff2);

  if (reader.u32() !== 0x774f4632)
    fail('not a WOFF2 file');

  const flavor = reader.u32();

  if (flavor === 0x74746366)
    fail('font collections are not supported');

  reader.u32(); // length
  const numTables = reader.u16();
  reader.u16(); // reserved
  reader.u32(); // totalSfntSize
  const compressedSize = reader.u32();
  reader.bytes(4 + 4 * 5); // version, metadata and private blocks

  const tables = [];

  for (let i = 0; i < numTables; ++i) {
    const flags = reader.u8();
    const tagIndex = flags & 0x3f;
    const tag = tagIndex === 0x3f ? reader.bytes(4).toString('latin1') : KNOWN_TAGS[tagIndex];
    const version = (flags >> 6) & 3;
    const origLength = reader.base128();
    const glyfOrLoca = tag === 'glyf' || tag === 'loca';
    const transformed = glyfOrLoca ? version === 0 : version !== 0;
    const length = transformed ? reader.base128() : origLength;
    tables.push({ tag, version, origLength, length, transformed });
  }

  const stream = zlib.brotliDecompressSync(reader.bytes(compressedSize));
  let offset = 0;

  for (const table of tables) {
    table.data = stream.subarray(offset, offset + table.length);
    offset += table.length;
    if (offset > stream.length)
      fail('tables run past the decompressed stream');
  }

  const byTag = new Map(tables.map((table) => [table.tag, table]));
  const glyfTable = byTag.get('glyf');
  const locaTable = byTag.get('loca');
  let rebuilt = null;

  if (glyfTable?.transformed) {
    if (!locaTable)
      fail('a transformed glyf without loca');
    rebuilt = reconstructGlyf(glyfTable.data, locaTable.origLength);
    glyfTable.data = rebuilt.glyf;
    locaTable.data = rebuilt.loca;
  }

  const hmtx = byTag.get('hmtx');

  if (hmtx?.transformed) {
    if (hmtx.version !== 1)
      fail(`hmtx transform ${hmtx.version} is not defined`);
    if (!rebuilt)
      fail('a transformed hmtx needs a transformed glyf');
    const numHMetrics = byTag.get('hhea').data.readUInt16BE(34);
    hmtx.data = reconstructHmtx(hmtx.data, numHMetrics, rebuilt.numGlyphs, rebuilt.xMins);
  }

  const head = byTag.get('head');

  if (head) {
    head.data = Buffer.from(head.data);
    head.data.writeUInt32BE(0, 8); // checksumAdjustment, set below
    if (rebuilt)
      head.data.writeInt16BE(rebuilt.indexFormat, 50);
  }

  // The sfnt: offset table, then the table records in tag order, then each table on 4 bytes.
  const sorted = [...tables].sort((a, b) => (a.tag < b.tag ? -1 : a.tag > b.tag ? 1 : 0));
  const entrySelector = Math.floor(Math.log2(numTables));
  const searchRange = 16 << entrySelector;
  const headerLength = 12 + 16 * numTables;
  const header = Buffer.alloc(headerLength);
  header.writeUInt32BE(flavor, 0);
  header.writeUInt16BE(numTables, 4);
  header.writeUInt16BE(searchRange, 6);
  header.writeUInt16BE(entrySelector, 8);
  header.writeUInt16BE(numTables * 16 - searchRange, 10);

  const body = new Writer();
  let headOffset = -1;

  sorted.forEach((table, i) => {
    const record = 12 + 16 * i;
    const tableOffset = headerLength + body.length;
    header.write(table.tag, record, 4, 'latin1');
    header.writeUInt32BE(checksum(table.data), record + 4);
    header.writeUInt32BE(tableOffset, record + 8);
    header.writeUInt32BE(table.data.length, record + 12);
    if (table.tag === 'head')
      headOffset = tableOffset;
    body.push(table.data);
    body.pad4();
  });

  const font = Buffer.concat([header, body.toBuffer()]);

  if (headOffset >= 0)
    font.writeUInt32BE((0xb1b0afba - checksum(font)) >>> 0, headOffset + 8);

  return font;
}
