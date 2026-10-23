#!/usr/bin/env node
/* debug-draw.js - 测量 ROM 绘制全屏文字需要多少帧 */
const fs = require('fs');
const path = require('path');
const Gameboy = require('serverboy');

const ROOT = path.resolve(__dirname, '..', '..');
const rom = fs.readFileSync(path.join(ROOT, 'build', 'passmgr.gb'));

const gb = new Gameboy();
gb.loadRom(rom);

function mapSnapshot() {
    const m = gb.getMemory(0x9800, 0x9C00);
    return Array.from(m).slice(0, 20 * 18).join(',');
}

let stableSince = -1;
let last = '';
let drawnAt = -1;

for (let f = 1; f <= 400; f++) {
    gb.doFrame();
    const snap = mapSnapshot();
    if (snap !== last) {
        last = snap;
        stableSince = f;
        if (drawnAt < 0) drawnAt = f;
    }
    // 标题行 (y=3) 第一个 tile
    const t = gb.getMemory(0x9800 + 3 * 32 + 2, 0x9800 + 3 * 32 + 3)[0];
    if (f <= 200 && f % 10 === 0) {
        console.log('frame %d: 标题行 tile=%d 变化帧=%d', f, t, stableSince);
    }
}

console.log('第一次出现非空绘制: 帧', drawnAt);
console.log('最后一次变化: 帧', stableSince);
console.log('标题行 tile 值:', gb.getMemory(0x9800 + 3 * 32 + 2, 0x9800 + 3 * 32 + 12));
