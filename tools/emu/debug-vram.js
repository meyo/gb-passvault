#!/usr/bin/env node
/* debug-vram.js - 挖 serverboy 内部 VRAM 结构 */
const fs = require('fs');
const path = require('path');
const Gameboy = require('serverboy');

const ROOT = path.resolve(__dirname, '..', '..');
const rom = fs.readFileSync(path.join(ROOT, 'build', 'passmgr.gb'));

// 在 ROM 里找 'A' 字形的字节序列 (0x20 00 50 00 A8 00 88 ...)
// 先构造我们期望的 A 字形: 见 genfont ('A' = ..#../.#.#./#...#/#...#/#####/#...#/#...#)
const aRows = [0b00100, 0b01010, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001];
const aBytes = [];
for (const r of aRows) { aBytes.push((r << 2) & 0xFF, 0x00); }
aBytes.push(0, 0);
const needle = Buffer.from(aBytes);
const idx = rom.indexOf(needle);
console.log("'A' 字形在 ROM 文件偏移:", idx >= 0 ? '0x' + idx.toString(16) : '未找到');
const idx2 = rom.indexOf(needle, idx + 1);
console.log("第二次出现:", idx2 >= 0 ? '0x' + idx2.toString(16) : '未找到');

const gb = new Gameboy();
gb.loadRom(rom);

const privKey = Object.getOwnPropertyNames(gb).find(k => k.startsWith('_'));
const core = gb[privKey].gameboy;
console.log('core keys:', Object.keys(core).filter(k => /vram|VRAM|CHR|mem/i.test(k)).join(', '));

function dumpCore(tag) {
    const mem = core.memory;
    const h = (arr, off, n) => Array.from(arr.slice(off, off + n)).map(b => b.toString(16).padStart(2, '0')).join(' ');
    console.log('---', tag, '---');
    console.log('memory[0x8000..0x8020]:', h(mem, 0x8000, 32));
    console.log('memory[0x9000..0x9020]:', h(mem, 0x9000, 32));
    console.log('memory[0x9800..0x9820]:', h(mem, 0x9800, 32));
    if (core.BGCHRBank1) {
        console.log('BGCHRBank1[0x000..0x020]:', h(core.BGCHRBank1, 0, 32));
        console.log('BGCHRBank1[0x400..0x420]:', h(core.BGCHRBank1, 0x400, 32));
        console.log('BGCHRBank1.length:', core.BGCHRBank1.length);
    } else {
        console.log('BGCHRBank1 不存在');
    }
    console.log('LCDC:', mem[0xFF40].toString(16), ' modeSTAT:', core.modeSTAT);
}

for (let i = 0; i < 5; i++) gb.doFrame();
dumpCore('5 帧');
for (let i = 0; i < 55; i++) gb.doFrame();
dumpCore('60 帧');
for (let i = 0; i < 60; i++) gb.doFrame();
dumpCore('120 帧');

// 屏幕像素: 找文字所在行
const scr = gb.getScreen();
function px(x, y) { const i = (y * 160 + x) * 4; return scr[i]; }
let rowInfo = [];
for (let ty = 0; ty < 18; ty++) {
    let dark = 0;
    for (let x = 0; x < 160; x++)
        for (let y = ty * 8; y < ty * 8 + 8; y++) if (px(x, y) < 128) dark++;
    if (dark > 0) rowInfo.push('y=' + ty + '(' + dark + 'px)');
}
console.log('有内容的文本行:', rowInfo.join(' '));
