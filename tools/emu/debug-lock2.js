#!/usr/bin/env node
/*
 * debug-lock2.js — 在 LY 读取瞬间抓栈, 直读 prev_ly / j / ticks
 *   压栈布局 (锁定页循环, PC 0x1473 处): SP+0=prev_ly SP+1=j SP+2=ly SP+3/4=ticks
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

/* LY 读取探针 */
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
                tLo: core.memory[(sp + 3) & 0xFFFF],
                tHi: core.memory[(sp + 4) & 0xFFFF]
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
/* 按住四键 6 帧 */
for (let f = 0; f < 6; f++) {
    gb.pressKey('A'); gb.pressKey('B'); gb.pressKey('SELECT'); gb.pressKey('START');
    gb.doFrame();
}
console.log('探针数: ' + probes.length);
const edges = probes.filter(e => e.ly >= 144 && e.prev < 144);
console.log('边沿命中 (ly>=144 && prev<144): ' + edges.length);
const combos = edges.filter(e => (e.j & 0xF0) === 0xF0).length;
console.log('  其中组合键按住: ' + combos);
console.log('ticks 末值: ' + (probes.length ? probes[probes.length - 1].tHi * 256 + probes[probes.length - 1].tLo : -1));
console.log();
console.log('最后一个 VBlank 进边沿前后 25 条 (ly,prev,j,ticks):');
const lastEdgeIdx = probes.map((e, i) => e.ly >= 144 && e.prev < 144 ? i : -1).filter(i => i >= 0).pop();
if (lastEdgeIdx >= 0) {
    for (let i = Math.max(0, lastEdgeIdx - 25); i <= Math.min(probes.length - 1, lastEdgeIdx + 5); i++) {
        const e = probes[i];
        const mark = (i === lastEdgeIdx) ? '  <== EDGE' : '';
        console.log(`  #${i}: ly=${e.ly} prev=${e.prev} j=0x${e.j.toString(16)} ticks=${e.tHi * 256 + e.tLo}${mark}`);
    }
}
console.log();
console.log('末 10 条探针:');
for (const e of probes.slice(-10)) console.log(`  ly=${e.ly} prev=${e.prev} j=0x${e.j.toString(16)} ticks=${e.tHi * 256 + e.tLo}`);
