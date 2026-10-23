#!/usr/bin/env node
/*
 * debug-lock.js — 追踪 SYSTEM LOCKED 页长按擦除不触发的原因
 *   1. 注入 tries=3 存档, 直达锁定页
 *   2. 包装 memoryReader: 记录 ROM 对 0xFF44(LY) / 0xFF00(JOYP) 的读取 (PC+值)
 *   3. 长按 A+B+SELECT+START 采样 PC, 判断: 循环在跑吗? LY 在变吗? 键位读到了吗?
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
const frames = n => { for (let i = 0; i < n; i++) gb.doFrame(); };
const tap = k => { gb.pressKey(k); gb.doFrame(); };
const tileAt = (x, y) => core.BGCHRBank1[(0x9800 + y * 32 + x) & 0x7FF];
const rowText = y => { let s = ''; for (let x = 0; x < 20; x++) s += String.fromCharCode(0x20 + (tileAt(x, y) & 63)); return s; };
const screenText = () => { const r = []; for (let y = 0; y < 18; y++) r.push(rowText(y)); return r.join('\n'); };
function waitUntil(fn, maxF = 400) { for (let f = 0; f < maxF; f++) { if (fn()) return true; gb.doFrame(); } return fn(); }
function tapUntil(k, fn, maxF = 150, retries = 8) { for (let t = 0; t < retries; t++) { tap(k); if (waitUntil(fn, maxF)) return true; } return false; }

/* 采样: 每 run() 后记 PC; keys 为按键数组 (每帧按住) */
function samplePC(nFrames, keys) {
    const pcs = [];
    for (let f = 0; f < nFrames; f++) {
        if (keys) for (const k of keys) gb.pressKey(k);
        for (let i = priv.pressed.length - 1; i >= 0; i--) if (priv.pressed[i]) priv.sendKey(i, true);
        core.frameDone = false;
        while (!core.frameDone) { core.run(); pcs.push(core.programCounter); }
        for (let i = priv.pressed.length - 1; i >= 0; i--) { priv.pressed[i] = false; priv.sendKey(i, false); }
    }
    return pcs;
}
const hx = v => '0x' + (v >>> 0).toString(16);

/* ---- 注入读取日志 (boot 后调用) ---- */
let lyLog = [], joyLog = [];
function installLoggers() {
    const origR44 = core.memoryReader[0xFF44], origH44 = core.memoryHighReader[0x44];
    const origR00 = core.memoryReader[0xFF00], origH00 = core.memoryHighReader[0x00];
    const wrap44 = fn => function (p, a) { const v = fn(p, a); if (lyLog.length < 3000) lyLog.push([core.programCounter, v]); return v; };
    const wrap00 = fn => function (p, a) { const v = fn(p, a); if (joyLog.length < 3000) joyLog.push([core.programCounter, v]); return v; };
    core.memoryReader[0xFF44] = wrap44(origR44); core.memoryHighReader[0x44] = wrap44(origH44);
    core.memoryReader[0xFF00] = wrap00(origR00); core.memoryHighReader[0x00] = wrap00(origH00);
}

/* ---- 流程 ---- */
boot(makeSram(3));
installLoggers();
if (!waitUntil(() => screenText().includes('PRESS START'), 300)) throw new Error('标题未出现');
if (!tapUntil('START', () => screenText().includes('SYSTEM LOCKED'), 400)) throw new Error('未进入锁定页');
console.log('已进入 SYSTEM LOCKED (tries=3) ✓');
frames(20);

console.log();
console.log('== 空闲 3 帧 PC 采样 ==');
{
    const pcs = samplePC(3);
    const hist = new Map(); for (const p of pcs) hist.set(p, (hist.get(p) || 0) + 1);
    console.log('PC: ' + [...hist.entries()].map(([p, c]) => hx(p) + 'x' + c).join(' '));
}
console.log('LY 读取: ' + lyLog.length + ' 次, 值域 ' + JSON.stringify([...new Set(lyLog.map(e => e[1]))].sort((a, b) => a - b)));
console.log('JOYP 读取: ' + joyLog.length + ' 次, 值域 ' + JSON.stringify([...new Set(joyLog.map(e => e[1]))].sort((a, b) => a - b)));

lyLog = []; joyLog = [];

console.log();
console.log('== 长按 A+B+SEL+START 40 帧 ==');
{
    const pcs = samplePC(40, ['A', 'B', 'SELECT', 'START']);
    const hist = new Map(); for (const p of pcs) hist.set(p, (hist.get(p) || 0) + 1);
    const top = [...hist.entries()].sort((a, b) => b[1] - a[1]).slice(0, 6);
    console.log('PC top: ' + top.map(([p, c]) => hx(p) + 'x' + c).join(' '));
}
const lyVals = [...new Set(lyLog.map(e => e[1]))].sort((a, b) => a - b);
console.log('LY 读取: ' + lyLog.length + ' 次, 值域 ' + JSON.stringify(lyVals));
const lyVbl = lyLog.filter(e => e[1] >= 144).length;
console.log('  其中 >=144 (VBlank) 次数: ' + lyVbl + ' / 40 帧');
const lyPCs = [...new Set(lyLog.map(e => e[0]))].map(hx);
console.log('  LY 读取处 PC: ' + lyPCs.join(' ') + (lyPCs.length > 12 ? ' ...' : ''));
const joyVals = [...new Set(joyLog.map(e => e[1]))].sort((a, b) => a - b);
console.log('JOYP 读取: ' + joyLog.length + ' 次, 值域 ' + JSON.stringify(joyVals));
const joyPCs = [...new Set(joyLog.map(e => e[0]))].map(hx);
console.log('  JOYP 读取处 PC: ' + joyPCs.slice(0, 12).join(' ') + (joyPCs.length > 12 ? ' ...' : ''));
console.log('  含全部按键的值 (0xF0 组合对应 raw 0xD0): ' + (joyVals.includes(0xD0) ? '出现 ✓' : '未出现 ✗'));

/* 继续长按到 230 帧总数, 看是否擦除 */
console.log();
console.log('== 继续长按至 230 帧 ==');
{
    const pcs = samplePC(190, ['A', 'B', 'SELECT', 'START']);
    const hist = new Map(); for (const p of pcs) hist.set(p, (hist.get(p) || 0) + 1);
    const top = [...hist.entries()].sort((a, b) => b[1] - a[1]).slice(0, 6);
    console.log('PC top: ' + top.map(([p, c]) => hx(p) + 'x' + c).join(' '));
}
frames(30);
console.log('当前画面:');
console.log(screenText().replace(/\s+$/gm, ''));
const st = gb.getSaveData();
console.log('SRAM: tries=' + st[8] + ' flags=0x' + st[10].toString(16) + ' slot0.used=0x' + st[16].toString(16));
console.log('core.halt=' + core.halt + ' PC=' + hx(core.programCounter) + ' LY=' + core.memory[0xFF44]);
