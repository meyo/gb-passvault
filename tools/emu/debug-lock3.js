#!/usr/bin/env node
/*
 * debug-lock3.js — 指令级追踪: LY 探针 + OPCODE 包装
 *   找出锁定页长按中每次 VBlank 边沿后 ~25k 周期的"空窗"究竟在执行什么
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
const hx = v => '0x' + (v >>> 0).toString(16);

/* ---- 指令追踪 ---- */
const trace = [];              // [pc, emulatorTicks]
let tracing = false;
function installTracer() {
    const orig = core.OPCODE;
    const wrapped = new Array(orig.length);
    for (let i = 0; i < orig.length; i++) {
        const fn = orig[i];
        wrapped[i] = function (p) {
            if (tracing && trace.length < 200000) trace.push([p.programCounter, p.emulatorTicks]);
            return fn(p);
        };
    }
    core.OPCODE = wrapped;
}

/* ---- LY 探针 ---- */
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
                tHi: core.memory[(sp + 4) & 0xFFFF],
                traceIdx: trace.length,
                ticks: core.emulatorTicks
            });
        }
        return v;
    };
}

boot(makeSram(3));
installTracer();
installProbe();
if (!waitUntil(() => screenText().includes('PRESS START'), 300)) throw new Error('标题未出现');
if (!tapUntil('START', () => screenText().includes('SYSTEM LOCKED'), 400)) throw new Error('未进入锁定页');
console.log('已进入 SYSTEM LOCKED ✓');

probes = []; trace.length = 0;
tracing = true;
for (let f = 0; f < 6; f++) {
    gb.pressKey('A'); gb.pressKey('B'); gb.pressKey('SELECT'); gb.pressKey('START');
    gb.doFrame();
}
tracing = false;
console.log('探针数: ' + probes.length + '  指令追踪数: ' + trace.length);

/* 找大跳跃 (ly 前进 > 10 行) */
for (let i = 1; i < probes.length; i++) {
    const d = probes[i].ly - probes[i - 1].ly;
    if (d > 10 || d < -100) {
        const a = probes[i - 1], b = probes[i];
        console.log();
        console.log(`=== 跳跃 #${i}: ly ${a.ly} -> ${b.ly}  (ticks ${a.ticks} -> ${b.ticks}, Δ${b.ticks - a.ticks} cycles) ===`);
        // 该区间的指令分布
        const lo = a.traceIdx, hi = b.traceIdx;
        const hist = new Map();
        for (let t = lo; t < hi; t++) hist.set(trace[t][0], (hist.get(trace[t][0]) || 0) + 1);
        const top = [...hist.entries()].sort((x, y) => y[1] - x[1]).slice(0, 12);
        console.log('  区间指令数: ' + (hi - lo) + ', 唯一 PC: ' + hist.size);
        console.log('  top PC: ' + top.map(([p, c]) => hx(p) + 'x' + c).join(' '));
        // 打印该区间的开头 40 条指令序列
        let line = [];
        for (let t = lo; t < Math.min(hi, lo + 40); t++) line.push(hx(trace[t][0]));
        console.log('  序列(前 40): ' + line.join(' '));
        line = [];
        for (let t = Math.max(lo, hi - 20); t < hi; t++) line.push(hx(trace[t][0]));
        console.log('  序列(后 20): ' + line.join(' '));
    }
}
console.log();
console.log('末 5 探针: ' + probes.slice(-5).map(e => `ly=${e.ly} prev=${e.prev} j=0x${e.j.toString(16)} ticks=${e.tHi * 256 + e.tLo}`).join(' | '));
