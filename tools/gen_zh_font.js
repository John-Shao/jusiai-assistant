#!/usr/bin/env node
// gen_zh_font.js — regenerate the subset CJK fonts for the UI.
//
// Scans src/i18n.cpp for every non-ASCII codepoint (the Chinese UI strings)
// and runs lv_font_conv to emit assets/fonts/lv_font_zh_<size>.c containing
// only ASCII 0x20-0x7F plus exactly those CJK glyphs — a few KB per size, so
// the binary stays small and no runtime font engine is needed.
//
// Regenerate whenever the Chinese strings in src/i18n.cpp change:
//   npm install -g lv_font_conv
//   node tools/gen_zh_font.js
//
// Override the source TTF with the FONT_TTF env var. SimHei is used by
// default because it ships with Windows; for a shipping product prefer an
// open-licensed CJK font (e.g. Noto Sans SC) — just point FONT_TTF at it.
'use strict';
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..');
const I18N = path.join(ROOT, 'src', 'i18n.cpp');
const OUT_DIR = path.join(ROOT, 'assets', 'fonts');
const TTF = process.env.FONT_TTF || 'C:/Windows/Fonts/simhei.ttf';
const SIZES = [14, 16, 20];

// Every non-ASCII codepoint in i18n.cpp is a UI glyph we must embed.
const src = fs.readFileSync(I18N, 'utf8');
const cps = new Set();
for (const ch of src) {
  const cp = ch.codePointAt(0);
  if (cp > 0x7f) cps.add(cp);
}
const sorted = [...cps].sort((a, b) => a - b);
console.log(`glyphs: ${sorted.length} CJK codepoints scanned from src/i18n.cpp`);

fs.mkdirSync(OUT_DIR, { recursive: true });

for (const size of SIZES) {
  const out = path.join(OUT_DIR, `lv_font_zh_${size}.c`);
  // Only ASCII hex reaches the command line — no CJK args, no encoding hazard.
  const args = [
    '--font', TTF,
    '--size', String(size),
    '--bpp', '4',
    '--format', 'lvgl',
    '--no-compress',
    '--lv-include', 'lvgl.h',
    '-r', '0x20-0x7F',
  ];
  for (const cp of sorted) args.push('-r', '0x' + cp.toString(16));
  args.push('-o', out);

  const r = spawnSync('lv_font_conv', args, { stdio: 'inherit', shell: true });
  if (r.status !== 0) {
    console.error(`FAILED: lv_font_conv exited ${r.status} for size ${size}`);
    process.exit(1);
  }
  console.log(`wrote ${out}`);
}
console.log('done.');
