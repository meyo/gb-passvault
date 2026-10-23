#!/usr/bin/env node
/*
 * debug-lock4.js — 全量探针转储: 每次 LY 读取时 (ly, prev, j, ticks)
 *   验证: 边沿 143->146 在实际序列里出现几次; ticks 在哪里递增
 */
const fs = require('fs');
const path = require('path');
const Gameboy = require('serverboy');

const ROOT = path.resolve(__dirname, '..', '..');
const rom = fs.readFileSync(path.join(ROOT, 'build', 'passmgr.gb'));

function makeSram(tries) {
    const s = new Uint8Array(32768);
    const put = (o, str) => { for (let i = 0; i < str.length; i++) s[o + i] = str.charCodeAt(i); };
    put(0, 'PMG1');
    s[4] = 8; s[5] = 9; s[6] = 0; s[7] = 1;
    s[8] = tries; s[10] = 1;
    const b = 16;
    s[b] = 0xA5; s[b + 1] = 5; put(b + 2, 'EMAIL');
    s[b + 10] = 3; put(b + 11, 'ABC');
    let c = 0xA5;
    for (let i = 0; i < 16; i++) if (i !== 9) c ^= s[i];
    for (let i = 0; i < 8 * 23; i++) c ^= s[16 + i];
    s[9] = c & 0xFF;
    return s;
}

let gb = null, priv = null, core = null;
function boot(save) {
    gb = new Gameboy();
    gb.loadRom(rom, save);
    priv = gb[Object.getOwnPropertyNames(gb).find(k => k.startsWith('_'))];
    core = priv.gameboy;
}
const tap = k => { gb.pressKey(k); gb.doFrame(); };
const tileAt = (x, y) => core.BGCHRBank1[(0x9800 + y * 32 + x) & 0x7FF];
const rowText = y => { let s = ''; for (let x = 0; x < 20; x++) s += String.fromCharCode(0x20 + (tileAt(x, y) & 63)); return s; };
const screenText = () => { const r = []; for (let y = 0; y < 18; y++) r.push(rowText(y)); return r.join('\n'); };
function waitUntil(fn, maxF = 400) { for (let f = 0; f < maxF; f++) { if (fn()) return true; gb.doFrame(); } return false; }
function tapUntil(k, fn, maxF = 150, retries = 8) { for (let t = 0; t < retries; t++) { tap(k); if (waitUntil(fn, maxF)) return true; } return false; }

let probes = [];
function installProbe() {
    const origH44 = core.memoryHighReader[0x44];
    core.memoryHighReader[0x44] = function (p, a) {
        const v = origH44(p, a);
        if (core.programCounter === 0x1474 && probes.length < 4000) {
            const sp = core.stackPointer;
            probes.push({
                ly: v,
                prev: core.memory[sp],
                j: core.memory[(sp + 1) & 0xFFFF],
                ticks: core.memory[(sp + 4) & 0xFFFF] * 256 + core.memory[(sp + 3) & 0xFFFF]
            });
        }
        return v;
    };
}

boot(makeSram(3));
installProbe();
if (!waitUntil(() => screenText().includes('PRESS START'), 300)) throw new Error('标题未出现');
if (!tapUntil('START', () => screenText().includes('SYSTEM LOCKED'), 400)) throw new Error('未进入锁定页');
console.log('已进入 SYSTEM LOCKED ✓');

probes = [];
for (let f = 0; f < 30; f++) {
    gb.pressKey('A'); gb.pressKey('B'); gb.pressKey('SELECT'); gb.pressKey('START');
    gb.doFrame();
}
console.log('探针数: ' + probes.length);

/* 压缩输出: 连续 +1 的 ly 序列折叠为范围; 显示 prev/j/ticks 变化 */
let out = [];
let i = 0;
let edges = 0, condEdges = 0, tickInc = 0;
for (i = 0; i < probes.length; i++) {
    const e = probes[i];
    if (i > 0) {
        const p = probes[i - 1];
        if (e.ly >= 144 && p.ly < 144) condEdges++;
        if (e.ticks > p.ticks) tickInc++;
    }
    if (e.ly >= 144 && e.prev < 144) edges++;
}
console.log('条件边沿 (ly>=144 && 前一条ly<144): ' + condEdges);
console.log('栈中边沿 (ly>=144 && prev<144): ' + edges);
console.log('ticks 递增次数 (相邻探针差值>0): ' + tickInc);
console.log();
console.log('序列 (每行 10 条: ly/prev/j/ticks):');
for (i = 0; i < probes.length; i += 10) {
    const seg = probes.slice(i, i + 10).map(e => `${e.ly},${e.prev},${e.j.toString(16)},${e.ticks}`).join('  ');
    console.log('  #' + String(i).padStart(4) + ': ' + seg);
}
