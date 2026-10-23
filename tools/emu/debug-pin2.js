#!/usr/bin/env node
/* debug-pin2.js - 单帧点按行为的逐步观察 */
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
function tap(key) { gb.pressKey(key); gb.doFrame(); }

frames(40);
console.log('title row11:', JSON.stringify(rowText(11)));
tap('START');
console.log('START 后每帧观察 row1 / row3:');
for (let f = 0; f < 160; f++) {
    gb.doFrame();
    const r1 = rowText(1), r3 = rowText(3);
    if (f % 20 === 0 || r1.includes('NEW PIN')) {
        console.log('f=' + f, JSON.stringify(r1), JSON.stringify(r3));
    }
    if (r1.includes('NEW PIN') && r3.includes('PIN:')) break;
}
console.log();
console.log('到 PIN 画面. 现在单帧 tap A, 观察后续 8 帧:');
tap('A');
for (let f = 0; f < 8; f++) {
    console.log('  +' + (f + 1) + '帧 row3 =', JSON.stringify(rowText(3)));
    gb.doFrame();
}
console.log('再 tap A 一次:');
tap('A');
for (let f = 0; f < 5; f++) {
    console.log('  +' + (f + 1) + '帧 row3 =', JSON.stringify(rowText(3)));
    gb.doFrame();
}
