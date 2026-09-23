#!/usr/bin/env node
/**
 * The shared effect presentation table, checked.
 *
 * core/ui/data/effect-presentation.json says how each effect and category looks, for both
 * Soundshed Guitar's web UI (through the generated core/ui/ts/generated/effectPresentation.ts)
 * and Soundshed Guitar Nano (core/src/uiclient/EffectPresentation.cpp, which reads it at run
 * time). Nothing else would notice a table entry that has drifted, so this fails when:
 *
 *   - an effect's name is not an EffectGuids constant in core/src/dsp/EffectGuids.h, or its
 *     guid differs from the constant's (a mistyped one silently gets the default look);
 *   - a category the table refers to (in the order, the aliases, the blend categories) is
 *     not defined;
 *   - a colour is not one both readers parse (#rgb, #rrggbb, rgb(), rgba());
 *   - an icon has no file in core/ui/images/icons, or an image no file under core/ui;
 *   - the generated web module is out of date (tools/gen-effect-presentation.mjs).
 *
 *   node tools/check-effect-presentation.mjs
 */

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { DATA, isCurrent, readPresentation } from './gen-effect-presentation.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const UI = path.join(ROOT, 'core', 'ui');
const rel = (file) => path.relative(ROOT, file).split(path.sep).join('/');

const COLOUR = /^(#[0-9a-f]{3}|#[0-9a-f]{6}|rgba?\(\s*\d{1,3}\s*,\s*\d{1,3}\s*,\s*\d{1,3}\s*(,\s*(0|1|0?\.\d+|1\.0+)\s*)?\))$/i;

function effectGuids() {
  const header = fs.readFileSync(path.join(ROOT, 'core', 'src', 'dsp', 'EffectGuids.h'), 'utf8');
  return new Map([...header.matchAll(/constexpr const char\* (k\w+) = "([^"]+)";/g)].map((m) => [m[1], m[2]]));
}

function check(doc) {
  const problems = [];
  const categories = doc.categories ?? {};
  const order = doc.categoryOrder ?? [];
  const isMap = (value) => value && typeof value === 'object' && !Array.isArray(value);

  for (const key of ['categoryOrder', 'nodeCategoryAliases', 'nodeClasses', 'blendCategories', 'categories', 'effects', 'legacyEffectEquipmentImages']) {
    if (!(key in doc)) problems.push(`"${key}" is missing`);
  }
  for (const key of ['defaultNodeClass', 'defaultIcon', 'defaultBlendCategory']) {
    if (typeof doc[key] !== 'string' || !doc[key]) problems.push(`"${key}" must be a non-empty string`);
  }
  if (!Array.isArray(order)) problems.push('"categoryOrder" must be an array');
  for (const key of ['nodeCategoryAliases', 'nodeClasses', 'blendCategories', 'categories', 'effects', 'legacyEffectEquipmentImages']) {
    if (key in doc && !isMap(doc[key])) problems.push(`"${key}" must be an object`);
  }
  if (problems.length > 0) return problems;

  const icons = new Map();
  const images = new Map();
  const noteIcon = (icon, where) => icon && !icons.has(icon) && icons.set(icon, where);
  const noteImage = (image, where) => image && !images.has(image) && images.set(image, where);
  noteIcon(doc.defaultIcon, 'defaultIcon');

  for (const id of order) if (!categories[id]) problems.push(`categoryOrder: "${id}" is not in categories`);
  if (new Set(order).size !== order.length) problems.push('categoryOrder lists a category twice');
  for (const [from, to] of Object.entries(doc.nodeCategoryAliases)) {
    if (!categories[to]) problems.push(`nodeCategoryAliases.${from}: "${to}" is not in categories`);
  }
  for (const [from, to] of Object.entries(doc.nodeClasses)) {
    if (typeof to !== 'string' || !to) problems.push(`nodeClasses.${from} must be a class name`);
  }
  for (const [from, to] of Object.entries(doc.blendCategories)) {
    if (!order.includes(to)) problems.push(`blendCategories.${from}: "${to}" is not an FX library category (categoryOrder)`);
  }
  if (!order.includes(doc.defaultBlendCategory)) problems.push(`defaultBlendCategory: "${doc.defaultBlendCategory}" is not in categoryOrder`);

  for (const [id, entry] of Object.entries(categories)) {
    const where = `categories.${id}`;
    if (typeof entry.name !== 'string' || !entry.name) problems.push(`${where}.name is missing`);
    if (!COLOUR.test(entry.color ?? '')) problems.push(`${where}.color "${entry.color}" is not #rgb, #rrggbb, rgb() or rgba()`);
    if (typeof entry.icon !== 'string' || !entry.icon) problems.push(`${where}.icon is missing`);
    noteIcon(entry.icon, where);
    if ('visualBackground' in entry) {
      const stops = entry.visualBackground;
      if (!Array.isArray(stops) || stops.length !== 2 || !stops.every((stop) => COLOUR.test(stop))) {
        problems.push(`${where}.visualBackground must be two colours`);
      }
    }
    noteImage(entry.equipmentImage, where);
  }

  const guids = effectGuids();
  const seen = new Map();
  for (const [name, entry] of Object.entries(doc.effects)) {
    const where = `effects.${name}`;
    if (!guids.has(name)) problems.push(`${where} is not a constant in core/src/dsp/EffectGuids.h`);
    else if (entry.guid !== guids.get(name)) problems.push(`${where}.guid is ${entry.guid}, EffectGuids.h has ${guids.get(name)}`);
    if (seen.has(entry.guid)) problems.push(`${where} has the same guid as effects.${seen.get(entry.guid)}`);
    seen.set(entry.guid, name);
    if (!entry.icon && !entry.equipmentImage) problems.push(`${where} sets neither icon nor equipmentImage`);
    noteIcon(entry.icon, where);
    noteImage(entry.equipmentImage, where);
  }
  for (const [id, image] of Object.entries(doc.legacyEffectEquipmentImages)) noteImage(image, `legacyEffectEquipmentImages.${id}`);

  for (const [icon, where] of icons) {
    if (!fs.existsSync(path.join(UI, 'images', 'icons', `${icon}.svg`))) problems.push(`${where}: no icon core/ui/images/icons/${icon}.svg`);
  }
  for (const [image, where] of images) {
    if (image.startsWith('/') || image.includes('..') || !fs.existsSync(path.join(UI, image))) problems.push(`${where}: no image core/ui/${image}`);
  }

  if (!isCurrent(doc)) {
    problems.push('core/ui/ts/generated/effectPresentation.ts is out of date. Run: node tools/gen-effect-presentation.mjs');
  }
  return problems;
}

let doc;
try {
  doc = readPresentation();
} catch (error) {
  console.error(`[check-effect-presentation] ${rel(DATA)} does not parse: ${error.message}`);
  process.exit(1);
}

const problems = check(doc);
if (problems.length > 0) {
  console.error(`[check-effect-presentation] FAIL - ${problems.length} problem(s) in ${rel(DATA)}:`);
  for (const problem of problems) console.error(`  ${problem}`);
  process.exitCode = 1;
} else {
  console.log(`[check-effect-presentation] OK - ${Object.keys(doc.effects).length} effects and ${Object.keys(doc.categories).length} categories match EffectGuids.h, the icons and images, and the generated module.`);
}
