/**
 * Small edits to a TrueType font for tools/gen-nano-fonts.mjs.
 *
 *   import { renameFamily } from './sfnt.mjs';
 *   fs.writeFileSync(file, renameFamily(fs.readFileSync(file), 'Archivo'));
 */

function fail(message) {
  throw new Error(`sfnt: ${message}`);
}

function checksum(buffer) {
  let sum = 0;
  const padded = buffer.length % 4 ? Buffer.concat([buffer, Buffer.alloc(4 - (buffer.length % 4))]) : buffer;
  for (let i = 0; i < padded.length; i += 4)
    sum = (sum + padded.readUInt32BE(i)) >>> 0;
  return sum;
}

function readTables(font) {
  const count = font.readUInt16BE(4);
  const tables = [];
  for (let i = 0; i < count; ++i) {
    const record = 12 + 16 * i;
    const offset = font.readUInt32BE(record + 8);
    const length = font.readUInt32BE(record + 12);
    tables.push({ tag: font.toString('latin1', record, record + 4), data: font.subarray(offset, offset + length) });
  }
  return tables;
}

function writeFont(flavor, tables) {
  const count = tables.length;
  const entrySelector = Math.floor(Math.log2(count));
  const searchRange = 16 << entrySelector;
  const headerLength = 12 + 16 * count;
  const header = Buffer.alloc(headerLength);
  header.writeUInt32BE(flavor, 0);
  header.writeUInt16BE(count, 4);
  header.writeUInt16BE(searchRange, 6);
  header.writeUInt16BE(entrySelector, 8);
  header.writeUInt16BE(count * 16 - searchRange, 10);

  const parts = [];
  let offset = headerLength;
  let headOffset = -1;

  tables.forEach((table, i) => {
    const record = 12 + 16 * i;
    let data = table.data;
    if (table.tag === 'head') {
      data = Buffer.from(data);
      data.writeUInt32BE(0, 8);
      headOffset = offset;
    }
    header.write(table.tag, record, 4, 'latin1');
    header.writeUInt32BE(checksum(data), record + 4);
    header.writeUInt32BE(offset, record + 8);
    header.writeUInt32BE(data.length, record + 12);
    const padding = Buffer.alloc((4 - (data.length % 4)) % 4);
    parts.push(data, padding);
    offset += data.length + padding.length;
  });

  const font = Buffer.concat([header, ...parts]);
  if (headOffset >= 0)
    font.writeUInt32BE((0xb1b0afba - checksum(font)) >>> 0, headOffset + 8);
  return font;
}

function encodeName(platform, text) {
  if (platform === 1)
    return Buffer.from(text, 'latin1');
  const buffer = Buffer.alloc(text.length * 2);
  for (let i = 0; i < text.length; ++i)
    buffer.writeUInt16BE(text.charCodeAt(i), i * 2);
  return buffer;
}

/**
 * `font` with its family name (name ID 1) set to `family`, its full name (4) to "<family>
 * Regular", and the typographic family and style names (16, 17) dropped, so that every cut
 * of one variable font joins one family, told apart by its OS/2 weight class, as the Inter
 * cuts are. Some web subsets name their family after their default instance ("Archivo
 * SemiBold"), which the web never sees, since @font-face names the family itself.
 */
export function renameFamily(font, family) {
  const tables = readTables(font);
  const name = tables.find((table) => table.tag === 'name');

  if (!name)
    fail('no name table');

  const data = name.data;
  const format = data.readUInt16BE(0);

  if (format !== 0)
    fail(`name table format ${format} is not handled`);

  const count = data.readUInt16BE(2);
  const storage = data.readUInt16BE(4);
  const records = [];

  for (let i = 0; i < count; ++i) {
    const at = 6 + 12 * i;
    const record = {
      platform: data.readUInt16BE(at),
      encoding: data.readUInt16BE(at + 2),
      language: data.readUInt16BE(at + 4),
      id: data.readUInt16BE(at + 6),
    };
    const length = data.readUInt16BE(at + 8);
    const offset = storage + data.readUInt16BE(at + 10);
    record.bytes = data.subarray(offset, offset + length);

    if (record.id === 16 || record.id === 17)
      continue;
    if (record.id === 1)
      record.bytes = encodeName(record.platform, family);
    else if (record.id === 4)
      record.bytes = encodeName(record.platform, `${family} Regular`);
    records.push(record);
  }

  const headerLength = 6 + 12 * records.length;
  const table = Buffer.alloc(headerLength);
  table.writeUInt16BE(0, 0);
  table.writeUInt16BE(records.length, 2);
  table.writeUInt16BE(headerLength, 4);
  let stringOffset = 0;

  records.forEach((record, i) => {
    const at = 6 + 12 * i;
    table.writeUInt16BE(record.platform, at);
    table.writeUInt16BE(record.encoding, at + 2);
    table.writeUInt16BE(record.language, at + 4);
    table.writeUInt16BE(record.id, at + 6);
    table.writeUInt16BE(record.bytes.length, at + 8);
    table.writeUInt16BE(stringOffset, at + 10);
    stringOffset += record.bytes.length;
  });

  name.data = Buffer.concat([table, ...records.map((record) => record.bytes)]);
  return writeFont(font.readUInt32BE(0), tables);
}
