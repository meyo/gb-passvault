#!/usr/bin/env node
/* debug-boot.js - 检查 ROM 启动阶段实际状态 */
const fs = require('fs');
const path = require('path');
const Gameboy = require('serverboy');

const ROOT = path.resolve(__dirname, '..', '..');
const rom = fs.readFileSync(path.join(ROOT, 'build', 'passmgr.gb'));

const gb = new Gameboy();
gb.loadRom(rom);

let curFrames = 0;
function runTo(n) { while (curFrames < n) { gb.doFrame(); curFrames++; } }

function hex(a, n) {
    const m = gb.getMemory(a, a + n);
    return Array.from(m).map(b => b.toString(16).padStart(2, '0')).join(' ');
}

const fontOff = 0x19AA;   // .map 中 _font_tiles 地址 (bank0 = 文件偏移)

runTo(5);
console.log('== 5 帧 ==');
console.log('LCDC (0xFF40):', hex(0xFF40, 1), ' 期望 bit7=0(关机) 或开机后 0x91');
console.log('VRAM tile0   :', hex(0x8000, 16));
console.log('ROM  font[0] :', Array.from(rom.slice(fontOff, fontOff + 16)).map(b => b.toString(16).padStart(2, '0')).join(' '));
console.log('ROM 入口 0x100:', Array.from(rom.slice(0x100, 0x108)).map(b => b.toString(16).padStart(2, '0')).join(' '));

runTo(30);
console.log();
console.log('== 30 帧 ==');
console.log('LCDC (0xFF40):', hex(0xFF40, 1));
console.log('BGP  (0xFF47):', hex(0xFF47, 1));
console.log('VRAM tile0   :', hex(0x8000, 16));
console.log('VRAM tile1   :', hex(0x8010, 16));
console.log('map 0x9800   :', hex(0x9800, 40));
console.log('map 0x9820   :', hex(0x9820, 40));

runTo(120);
console.log();
console.log('== 120 帧 ==');
console.log('LCDC:', hex(0xFF40, 1));
console.log('VRAM tile0:', hex(0x8000, 16));
console.log('map 0x9800:', hex(0x9800, 40));

const scr = gb.getScreen();
let nonzero = 0;
let firstPx = null;
for (let i = 0; i < scr.length; i += 4) {
    if (scr[i] < 200 || scr[i + 1] < 200 || scr[i + 2] < 200) { nonzero++; if (!firstPx) firstPx = i / 4; }
}
console.log('屏幕暗像素数:', nonzero, ' 第一处:', firstPx);
