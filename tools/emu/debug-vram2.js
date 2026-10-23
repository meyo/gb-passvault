#!/usr/bin/env node
/* debug-vram2.js - 确认 tilemap 在 BGCHRBank1 中的位置与出现时间 */
const fs = require('fs');
const path = require('path');
const Gameboy = require('serverboy');

const ROOT = path.resolve(__dirname, '..', '..');
const rom = fs.readFileSync(path.join(ROOT, 'build', 'passmgr.gb'));

const gb = new Gameboy();
gb.loadRom(rom);
const privKey = Object.getOwnPropertyNames(gb).find(k => k.startsWith('_'));
const core = gb[privKey].gameboy;

function mapChar(x, y) {
    const t = core.BGCHRBank1[(0x9800 + y * 32 + x) & 0x7FF];
    return String.fromCharCode(0x20 + (t & 63));
}

function firstTextFrame() {
    for (let f = 1; f <= 300; f++) {
        gb.doFrame();
        if (mapChar(2, 3) === 'G' && mapChar(3, 3) === 'B') return f;
    }
    return -1;
}

const f = firstTextFrame();
console.log('标题 "GB" 首次出现于第', f, '帧');
console.log();
console.log('tilemap (BGCHRBank1) 文本:');
for (let y = 0; y < 18; y++) {
    let s = '';
    for (let x = 0; x < 20; x++) s += mapChar(x, y);
    console.log(('' + y).padStart(2) + '|' + s.replace(/\s+$/g, '') + '|');
}

// 记录两帧之间 tile 的变化数, 评估绘制速度
const snapBefore = Array.from(core.BGCHRBank1.slice(0, 0x400));
for (let i = 0; i < 2; i++) gb.doFrame();
const snapAfter = Array.from(core.BGCHRBank1.slice(0, 0x400));
let changed = 0;
for (let i = 0; i < 0x400; i++) if (snapBefore[i] !== snapAfter[i]) changed++;
console.log('两帧之间 tilemap 变化字节数:', changed, '(0 = 已静止, 等待按键中)');
