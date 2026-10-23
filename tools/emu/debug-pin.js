#!/usr/bin/env node
/* debug-pin.js - 精确观察 PIN 画面按键行为 */
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
function press(key, n) { n = n || 12; for (let i = 0; i < n; i++) { gb.pressKey(key); gb.doFrame(); } }

frames(40);
console.log('标题画面 row11:', JSON.stringify(rowText(11)));
press('START');
frames(250);
console.log('设置 PIN 画面:');
console.log('  row1 :', JSON.stringify(rowText(1)));
console.log('  row3 :', JSON.stringify(rowText(3)));
console.log('  高亮格(6,6) tile =', core.BGCHRBank1[(0x9800 + 6 * 32 + 6) & 0x7FF]);

console.log('按 A (12 帧)...');
press('A');
frames(30);
console.log('  row3 :', JSON.stringify(rowText(3)));

console.log('按 A 再两次...');
press('A'); frames(20);
press('A'); frames(20);
console.log('  row3 :', JSON.stringify(rowText(3)));

console.log('按 RIGHT 再 A...');
press('RIGHT'); frames(20);
console.log('  高亮(10,6) tile =', core.BGCHRBank1[(0x9800 + 6 * 32 + 10) & 0x7FF], ' (6,6) tile =', core.BGCHRBank1[(0x9800 + 6 * 32 + 6) & 0x7FF]);
press('A'); frames(30);
console.log('  row3 :', JSON.stringify(rowText(3)));

console.log('按键后 P1 寄存器 (0xFF00):', gb.getMemory(0xFF00, 0xFF01)[0].toString(16));
