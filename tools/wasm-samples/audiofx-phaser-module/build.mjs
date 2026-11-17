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

function readDescriptorText(source) {
  const dataMatch = source.match(/\(data\s+\(i32\.const\s+0\)((?:\s*"(?:\.|[^"])*")+)\s*\)/);

  if (!dataMatch) {
    throw new Error("Could not locate the descriptor data segment at address 0.");
  }

  return [...dataMatch[1].matchAll(/"((?:\.|[^"])*)"/g)].map((match) => decodeWatString(match[1])).join("");
}

function readDeclaredDescriptorLength(source) {
  const lengthMatch = source.match(/\(func\s+\(export\s+"audiofx_descriptor_len"\)[\s\S]*?i32\.const\s+(\d+)/);

  if (!lengthMatch) {
    throw new Error("Could not locate audiofx_descriptor_len.");
  }

  return Number(lengthMatch[1]);
}

// The param ids the descriptor declares, in declaration order, with their defaults.
function readDeclaredParams(descriptorText) {
  const params = new Map();

  for (const line of descriptorText.split("\n")) {
    const match = line.match(/^param\.([^.=]+)\.([^=]+)=(.*)$/);

    if (!match) {
      continue;
    }

    if (!params.has(match[1])) {
      params.set(match[1], 0);
    }

    if (match[2] === "default") {
      params.set(match[1], Number(match[3]));
    }
  }

  return params;
}

// Runs the module the way the host does (Custom Effect ABI 2): prepare, look the buffer up, process
// blocks of a sine in place. Handles come from an arbitrary base, as the host's do, so a module
// that hard-codes them fails here too.
async function smokeTest(compiledModule, params) {
  const handleBase = 7000;
  const ids = [...params.keys()];
  let memory;
  const readId = (pointer, length) => new TextDecoder().decode(new Uint8Array(memory.buffer, pointer, length));
  const instance = await WebAssembly.instantiate(compiledModule, {
    host: {
      param_handle: (pointer, length) => {
        const index = ids.indexOf(readId(pointer, length));
        return index < 0 ? -1 : handleBase + index;
      },
      param_value: (handle) => {
        const id = ids[handle - handleBase];

        if (id === undefined) {
          throw new Error(`param_value was given handle ${handle}, which param_handle did not return.`);
        }

        return params.get(id);
      },
      tempo_bpm: () => 120,
      resource_handle: () => -1,
      resource_size: () => 0,
      resource_read: () => 0,
    },
  });
  const guest = instance.exports;
  memory = guest.memory;

  if (guest.audiofx_abi_version() !== 2) {
    throw new Error(`audiofx_abi_version returned ${guest.audiofx_abi_version()}, not 2.`);
  }

  const maxBlock = 512;
  const status = guest.audiofx_prepare(48000, maxBlock);

  if (status !== 0) {
    throw new Error(`audiofx_prepare returned ${status}.`);
  }

  const bufferPointer = guest.audiofx_buffer_ptr();

  if (bufferPointer % 4 !== 0 || bufferPointer + maxBlock * 8 > memory.buffer.byteLength) {
    throw new Error(`audiofx_buffer_ptr returned ${bufferPointer}, which is unaligned or doesn't fit in memory.`);
  }

  let peak = 0;

  for (let start = 0; start < 48000; start += 300) {
    const io = new Float32Array(memory.buffer, bufferPointer, maxBlock * 2);

    for (let i = 0; i < 300; i += 1) {
      const sample = 0.5 * Math.sin((2 * Math.PI * 440 * (start + i)) / 48000);
      io[i] = sample;
      io[maxBlock + i] = sample;
    }

    guest.audiofx_process(300);
    const out = new Float32Array(memory.buffer, bufferPointer, maxBlock * 2);

    for (let i = 0; i < 300; i += 1) {
      for (const value of [out[i], out[maxBlock + i]]) {
        if (!Number.isFinite(value)) {
          throw new Error(`Output sample ${start + i} is not finite.`);
        }

        peak = Math.max(peak, Math.abs(value));
      }
    }
  }

  return peak;
}

const wabt = await wabtFactory();
const source = await readFile(sourcePath, "utf8");
const descriptorText = readDescriptorText(source);
const descriptorByteLength = Buffer.byteLength(descriptorText, "utf8");
const declaredDescriptorLength = readDeclaredDescriptorLength(source);

if (descriptorByteLength !== declaredDescriptorLength) {
  throw new Error(
    `Descriptor length mismatch: declared ${declaredDescriptorLength}, actual ${descriptorByteLength}.`,
  );
}

const moduleHandle = wabt.parseWat(path.basename(sourcePath), source);
const { buffer } = moduleHandle.toBinary({
  log: false,
  write_debug_names: true,
});

await mkdir(path.dirname(outputPath), { recursive: true });
await writeFile(outputPath, Buffer.from(buffer));

const compiledModule = await WebAssembly.compile(buffer);
const exportsList = WebAssembly.Module.exports(compiledModule).map(({ kind, name }) => `${kind}:${name}`);
const exportNames = new Set(exportsList.map((entry) => entry.split(":")[1]));

for (const requiredExport of [
  "memory",
  "audiofx_abi_version",
  "audiofx_descriptor_ptr",
  "audiofx_descriptor_len",
  "audiofx_prepare",
  "audiofx_buffer_ptr",
  "audiofx_process",
  "audiofx_reset",
]) {
  if (!exportNames.has(requiredExport)) {
    throw new Error(`Missing required export: ${requiredExport}`);
  }
}

const peak = await smokeTest(compiledModule, readDeclaredParams(descriptorText));

console.log(`Built ${path.relative(rootDir, outputPath)}`);
console.log(`Descriptor bytes: ${descriptorByteLength}`);
console.log(`Exports: ${exportsList.join(", ")}`);
console.log(`Smoke test: one second of a 440 Hz sine at the defaults, peak ${peak.toFixed(3)}, all finite.`);
