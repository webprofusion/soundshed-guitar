import { mkdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import wabtFactory from "wabt";

const rootDir = path.dirname(fileURLToPath(import.meta.url));
const sourcePath = path.join(rootDir, "ai_phaser.wat");
const outputPath = path.join(rootDir, "dist", "ai_phaser.wasm");

function decodeWatString(encoded) {
  let decoded = "";

  for (let index = 0; index < encoded.length; index += 1) {
    const char = encoded[index];

    if (char !== "\\") {
      decoded += char;
      continue;
    }

    index += 1;

    switch (encoded[index]) {
      case "n":
        decoded += "\n";
        break;
      case "r":
        decoded += "\r";
        break;
      case "t":
        decoded += "\t";
        break;
      case '"':
        decoded += '"';
        break;
      case "\\":
        decoded += "\\";
        break;
      default:
        throw new Error(`Unsupported WAT string escape: \\${encoded[index]}`);
    }
  }

  return decoded;
}

function readDescriptorByteLength(source) {
  const dataMatch = source.match(/\(data\s+\(i32\.const\s+0\)([\s\S]*?)\)\s*\(global/);

  if (!dataMatch) {
    throw new Error("Could not locate the descriptor data segment.");
  }

  const strings = [...dataMatch[1].matchAll(/"((?:\\.|[^"])*)"/g)].map((match) => decodeWatString(match[1]));
  return Buffer.byteLength(strings.join(""), "utf8");
}

function readDeclaredDescriptorLength(source) {
  const lengthMatch = source.match(/\(func\s+\(export\s+"audiofx_descriptor_len"\)[\s\S]*?i32\.const\s+(\d+)/);

  if (!lengthMatch) {
    throw new Error("Could not locate audiofx_descriptor_len.");
  }

  return Number(lengthMatch[1]);
}

const wabt = await wabtFactory();
const source = await readFile(sourcePath, "utf8");
const descriptorByteLength = readDescriptorByteLength(source);
const declaredDescriptorLength = readDeclaredDescriptorLength(source);

if (descriptorByteLength !== declaredDescriptorLength) {
  throw new Error(
    `Descriptor length mismatch: declared ${declaredDescriptorLength}, actual ${descriptorByteLength}.`,
  );
}

const moduleHandle = wabt.parseWat(path.basename(sourcePath), source, {
  multi_value: true,
});
const { buffer } = moduleHandle.toBinary({
  log: false,
  write_debug_names: true,
});

await mkdir(path.dirname(outputPath), { recursive: true });
await writeFile(outputPath, Buffer.from(buffer));

const compiledModule = await WebAssembly.compile(buffer);
const exportsList = WebAssembly.Module.exports(compiledModule).map(({ kind, name }) => `${kind}:${name}`);
const exportNames = new Set(exportsList.map((entry) => entry.split(":")[1]));

for (const requiredExport of ["audiofx_prepare", "audiofx_reset", "audiofx_process"]) {
  if (!exportNames.has(requiredExport)) {
    throw new Error(`Missing required export: ${requiredExport}`);
  }
}

if ((exportNames.has("audiofx_descriptor_ptr") || exportNames.has("audiofx_descriptor_len")) && !exportNames.has("memory")) {
  throw new Error("Descriptor exports require an exported memory.");
}

console.log(`Built ${path.relative(rootDir, outputPath)}`);
console.log(`Descriptor bytes: ${descriptorByteLength}`);
console.log(`Exports: ${exportsList.join(", ")}`);