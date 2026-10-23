#!/usr/bin/env node
/* debug-timing.js - 量测 ROM 各阶段持续帧数 */
const fs = require('fs');
const path = require('path');
const Gameboy = require('serverboy');

const ROOT = path.resolve(__dirname, '..', '..');
const rom = fs.readFileSync(path.join(ROOT, 'build', 'passmgr.gb'));

const gb = new Gameboy();
gb.loadRom(rom);
const privKey = Object.getOwnPropertyNames(gb).find(k => k.startsWith('_'));
const core = gb[privKey].gameboy;

function rowText(y) { let s = ''; for (let x = 0; x < 20; x++) { const t = core.BGCHRBank1[(0x9800 + y * 32 + x) & 0x7FF]; s += String.fromCharCode(0x20 + (t & 63)); } return s; }
function frames(n) { for (let i = 0; i < n; i++) gb.doFrame(); }

frames(40);
gb.pressKey('START'); gb.doFrame();

let last = '';
for (let f = 0; f < 700; f++) {
    const sig = rowText(1).trim() + '|' + rowText(3).trim();
    if (sig !== last) {
        console.log('帧 ' + f + ': row1=' + JSON.stringify(rowText(1).trim()) + ' row3=' + JSON.stringify(rowText(3).trim()));
        last = sig;
    }
    gb.doFrame();
}

// 现在应该在 PIN 画面. 单帧 A.
console.log('--- 进入 PIN 画面后 ---');
console.log('tap A');
gb.pressKey('A'); gb.doFrame();
for (let f = 0; f < 6; f++) {
    console.log('  帧 ' + f + ': row3=' + JSON.stringify(rowText(3)));
    gb.doFrame();
}
