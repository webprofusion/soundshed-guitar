#!/usr/bin/env node
/**
 * Drives Soundshed Guitar Nano's native UI from scripts and agents: the counterpart of
 * cdp-tool.mjs for the WebView UI. Nano listens only when started with
 * SOUNDSHED_NANO_DEBUG_PORT set (see juce/source/nativeui/debug/NanoDebugServer.h).
 *
 *   SOUNDSHED_NANO_DEBUG_PORT=9444 "juce/builds/SoundshedGuitarNano_artefacts/Debug/Standalone/Soundshed Guitar Nano.exe" &
 *   node tools/agent-ui-debug/native/nano-tool.mjs 9444 state
 *   node tools/agent-ui-debug/native/nano-tool.mjs 9444 tree [--ids]
 *   node tools/agent-ui-debug/native/nano-tool.mjs 9444 click preset-next
 *   node tools/agent-ui-debug/native/nano-tool.mjs 9444 longpress node:amp_0
 *   node tools/agent-ui-debug/native/nano-tool.mjs 9444 screenshot out.png [scale]
 *   node tools/agent-ui-debug/native/nano-tool.mjs 9444 resize 800 480
 *   node tools/agent-ui-debug/native/nano-tool.mjs 9444 theme light
 *   node tools/agent-ui-debug/native/nano-tool.mjs 9444 menu                  (list the open popup menu)
 *   node tools/agent-ui-debug/native/nano-tool.mjs 9444 menu "Move later"     (press one of its items)
 *   node tools/agent-ui-debug/native/nano-tool.mjs 9444 combo settings-nam-oversampling 2x
 *   node tools/agent-ui-debug/native/nano-tool.mjs 9444 type prompt-text "My preset" --enter
 *   node tools/agent-ui-debug/native/nano-tool.mjs 9444 slider settings-scale 125
 *
 * Ids are component ids, or targets a view draws itself (NamedTargets): "node:amp_0",
 * "bypass:amp_0", "add:amp_0" on the chain, "fx:<effect name>" in the effect picker,
 * "preset:<name>" and "slot:<n>" in the preset and setlist lists, "tone:<title>", "installed:<title>"
 * and "action:<text>" on the Tones page.
 *   node tools/agent-ui-debug/native/nano-tool.mjs 9444 send '{"type":"selectScene","sceneId":"scene-2"}'
 *   node tools/agent-ui-debug/native/nano-tool.mjs 9444 raw '{"cmd":"tree"}'
 */

import net from 'node:net';
import path from 'node:path';

const [portArg, cmd, ...args] = process.argv.slice(2);
const port = Number(portArg);

if (!port || !cmd) {
  console.error('usage: nano-tool.mjs <port> <state|tree|click|longpress|screenshot|resize|theme|menu|combo|type|slider|send|raw> [args]');
  process.exit(2);
}

function request() {
  switch (cmd) {
    case 'state':
    case 'tree':
      return { cmd };
    case 'click':
    case 'longpress':
      return { cmd, id: args[0] };
    case 'screenshot':
      return { cmd, path: path.resolve(args[0] ?? 'nano.png'), scale: Number(args[1] ?? 1) };
    case 'resize':
      return { cmd, width: Number(args[0]), height: Number(args[1]) };
    case 'theme':
      return { cmd, name: args[0] };
    case 'menu':
      return { cmd, item: args[0] ?? '' };
    case 'combo':
      return { cmd, id: args[0], item: args[1] ?? '' };
    case 'slider':
      return { cmd, id: args[0], value: Number(args[1]) };
    case 'type':
      return { cmd, id: args[0], text: args[1] ?? '', enter: args.includes('--enter') };
    case 'send':
      return { cmd, message: JSON.parse(args[0]) };
    case 'raw':
      return JSON.parse(args[0]);
    default:
      throw new Error(`unknown command ${cmd}`);
  }
}

/** Visible components with an id, flattened: "id  type  x,y wxh  text". */
function listIds(node, out = []) {
  if (node.id) out.push(`${node.id.padEnd(32)} ${node.type.padEnd(22)} ${node.bounds.join(',')}  ${node.text ?? ''}${node.toggled ? ' [on]' : ''}`);
  for (const child of node.children ?? []) listIds(child, out);
  return out;
}

const socket = net.createConnection({ host: '127.0.0.1', port }, () => socket.write(`${JSON.stringify(request())}\n`));
let buffer = '';
socket.on('data', (chunk) => {
  buffer += chunk.toString('utf8');
  const newline = buffer.indexOf('\n');
  if (newline < 0) return;
  const reply = JSON.parse(buffer.slice(0, newline));
  socket.end();
  if (!reply.ok) {
    console.error(`[nano-tool] ${reply.error}`);
    process.exitCode = 1;
    return;
  }
  if (cmd === 'tree' && args.includes('--ids')) console.log(listIds(reply.tree).join('\n'));
  else console.log(JSON.stringify(reply, null, 2));
});
socket.on('error', (error) => {
  console.error(`[nano-tool] ${error.message}`);
  process.exitCode = 1;
});
