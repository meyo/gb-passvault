#!/usr/bin/env node
/*
 * debug-detail.js — 追踪详情页 B 键失效 / 疑似卡死
 *
 * 方法:
 *   1. 注入自造 SRAM (PIN 1234 + 槽位 EMAIL/ABC), 走正常解锁流程进入列表/详情
 *   2. 对照实验: 列表页 (getkey 忙轮询) 按 B → 预期回菜单
 *   3. 详情页 (wait_vbl_done 帧同步) 采样 PC + 中断寄存器, 判断:
 *        - 是否卡在 ROM 0x1BD..0x1CD (wait_vbl_done 的 HALT 循环)
 *        - IE/IF/IME/halt 状态, __vbl_done(0xFF91) 是否被置位
 *   4. 详情页按 B 采样: 画面是否变化 (回列表)
 */
const fs = require('fs');
const path = require('path');
const Gameboy = require('serverboy');

const ROOT = path.resolve(__dirname, '..', '..');
const rom = fs.readFileSync(path.join(ROOT, 'build', 'passmgr.gb'));

/* ---- 自造 SRAM: PIN 1234 + slot0 EMAIL/ABC ---- */
function makeSram() {
    const s = new Uint8Array(32768);
    const put = (o, str) => { for (let i = 0; i < str.length; i++) s[o + i] = str.charCodeAt(i); };
    put(0, 'PMG1');
    s[4] = 8; s[5] = 9; s[6] = 0; s[7] = 1;   /* 1234 obf (+7)%10 */
    s[8] = 0; s[10] = 1;                       /* tries=0 pin_set=1 */
    const b = 16;                              /* slot 0 */
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

/* ---- 复刻 interface.doFrame, 每个 run() 后采样一次 PC ---- */
function samplePC(nFrames, pressKey) {
    const pcs = [];
    for (let f = 0; f < nFrames; f++) {
        if (pressKey) gb.pressKey(pressKey);
        for (let i = priv.pressed.length - 1; i >= 0; i--) if (priv.pressed[i]) priv.sendKey(i, true);
        core.frameDone = false;
        while (!core.frameDone) { core.run(); pcs.push(core.programCounter); }
        for (let i = priv.pressed.length - 1; i >= 0; i--) { priv.pressed[i] = false; priv.sendKey(i, false); }
    }
    return pcs;
}
function hx(v) { return '0x' + (v >>> 0).toString(16); }
function state() {
    return {
        pc: hx(core.programCounter),
        halt: core.halt, stopped: core.CPUStopped,
        IME: core.IME, IE: hx(core.interruptsEnabled), IF: hx(core.interruptsRequested),
        irqMatch: core.IRQLineMatched, irqDelay: core.IRQEnableDelay,
        vblDone: core.memory[0xFF91], LY: core.memory[0xFF44], mode: core.modeSTAT,
        stop: core.stopEmulator
    };
}
function report(name, pcs) {
    const hist = new Map();
    for (const p of pcs) hist.set(p, (hist.get(p) || 0) + 1);
    const top = [...hist.entries()].sort((a, b) => b[1] - a[1]).slice(0, 10);
    const inRange = (lo, hi) => pcs.filter(p => p >= lo && p <= hi).length;
    console.log(`[${name}] 样本=${pcs.length} 唯一PC=${hist.size}`);
    console.log('   top: ' + top.map(([p, c]) => `${hx(p)}x${c}`).join(' '));
    console.log(`   在 wait_vbl 区(0x1BD-0x1CD): ${inRange(0x1BD, 0x1CD)}  | 在 joypad/waitpad 区(0x233B-0x2380): ${inRange(0x233B, 0x2380)}`);
}

/* ---- PIN 数字键盘 (从 smoke.js 复制) ---- */
const PAD_X = [6, 10, 14], PAD_Y = [6, 8, 10, 12];
function padPos(d) { if (d === 0) return { r: 3, c: 1 }; const i = d - 1; return { r: (i / 3) | 0, c: i % 3 }; }
function pinScreenReady(title) {
    return rowText(1).includes(title) && rowText(3).slice(0, 16) === '      PIN:  ____';
}
function pinFieldShown(digits) {
    const f = digits + '_'.repeat(4 - digits.length);
    return rowText(3).slice(12, 16) === f;
}
function padHighlighted(c, r) { return tileAt(PAD_X[c], PAD_Y[r]) >= 64; }
function enterPin(pin) {
    let cur = { r: 0, c: 0 };
    const digits = [];
    for (let i = 0; i < pin.length; i++) {
        const t = padPos(+pin[i]);
        const moves = [];
        while (cur.r < t.r) { cur = { r: cur.r + 1, c: cur.c }; moves.push(['DOWN', cur]); }
        while (cur.r > t.r) { cur = { r: cur.r - 1, c: cur.c }; moves.push(['UP', cur]); }
        while (cur.c < t.c) { cur = { r: cur.r, c: cur.c + 1 }; moves.push(['RIGHT', cur]); }
        while (cur.c > t.c) { cur = { r: cur.r, c: cur.c - 1 }; moves.push(['LEFT', cur]); }
        for (const [key, hl] of moves) {
            if (!tapUntil(key, () => padHighlighted(hl.c, hl.r), 60, 5)) throw new Error('PIN 光标未移动');
        }
        digits.push(pin[i]);
        if (i < pin.length - 1) {
            if (!tapUntil('A', () => pinFieldShown(digits.join('')), 60, 5)) throw new Error('PIN 字段未显示 ' + digits.join(''));
        } else {
            tap('A');
        }
    }
}

/* ---- 菜单/列表 (从 smoke.js 复制) ---- */
function menuSel() { for (let i = 0; i < 4; i++) if (tileAt(2, 6 + i * 2) >= 64) return i; return -1; }
function listSel() { for (let i = 0; i < 8; i++) if (tileAt(1, 3 + i) >= 64) return i; return -1; }
function menuGoto(i) {
    if (!waitUntil(() => menuSel() >= 0, 500)) throw new Error('菜单高亮未找到');
    let cur = menuSel();
    while (cur < i) { if (!tapUntil('DOWN', () => menuSel() === cur + 1, 60, 5)) throw new Error('菜单未下移'); cur++; }
    while (cur > i) { if (!tapUntil('UP', () => menuSel() === cur - 1, 60, 5)) throw new Error('菜单未上移'); cur--; }
}
function listGoto(i) {
    if (!waitUntil(() => listSel() >= 0, 500)) throw new Error('列表高亮未找到');
    let cur = listSel();
    while (cur < i) { if (!tapUntil('DOWN', () => listSel() === cur + 1, 60, 5)) throw new Error('列表未下移'); cur++; }
    while (cur > i) { if (!tapUntil('UP', () => listSel() === cur - 1, 60, 5)) throw new Error('列表未上移'); cur--; }
}

/* ================================================================== */
boot(makeSram());

/* 1. 解锁 */
if (!waitUntil(() => screenText().includes('PRESS START'), 300)) throw new Error('标题未出现');
if (!tapUntil('START', () => pinScreenReady('ENTER PIN'), 300, 8)) throw new Error('未进入解锁画面');
console.log('解锁画面 OK');
enterPin('1234');
if (!waitUntil(() => screenText().includes('VAULT MENU'), 500)) {
    console.log('*** 解锁失败? 当前屏幕 ***');
    console.log(screenText());
    process.exit(1);
}
console.log('自造 SRAM 注入 + PIN 1234 解锁成功 ✓');
console.log();

/* 2. 对照: 列表页按 B (getkey 路径) */
menuGoto(0);
if (!tapUntil('A', () => screenText().includes('ACCOUNTS'), 200, 6)) throw new Error('未进入账号列表');
frames(30);
report('列表页空闲', samplePC(2, null));
console.log('列表页状态: ' + JSON.stringify(state()));
const backToMenu = tapUntil('B', () => screenText().includes('VAULT MENU'), 200, 6);
console.log('对照结果: 列表页按 B ' + (backToMenu ? '成功返回菜单 ✓ (B 键通路 OK)' : '失败 ✗ (B 键本身不通!)'));
if (!backToMenu) { console.log(screenText()); process.exit(1); }
console.log();

/* 3. 进详情页, 采样 */
menuGoto(0);
if (!tapUntil('A', () => screenText().includes('ACCOUNTS'), 200, 6)) throw new Error('未进入账号列表');
listGoto(0);
if (!tapUntil('A', () => screenText().includes('SLOT DETAIL'), 200, 6)) throw new Error('未进入详情页');
console.log('已进入详情页, 等待绘制稳定...');
frames(120);
console.log('详情页画面:');
console.log(screenText().replace(/\s+$/gm, ''));
console.log('详情页状态: ' + JSON.stringify(state()));
console.log();

const pcs0 = samplePC(3, null);
report('详情页空闲', pcs0);
console.log('空闲后状态: ' + JSON.stringify(state()));
console.log();

const pcsB = samplePC(3, 'B');
report('详情页按B中', pcsB);
console.log('按B后状态: ' + JSON.stringify(state()));
const afterText = screenText();
console.log('按 B 后画面含 ACCOUNTS? ' + afterText.includes('ACCOUNTS') + '  含 SLOT DETAIL? ' + afterText.includes('SLOT DETAIL'));
console.log();

/* 4. 再等一会, 看是否有变化 */
frames(60);
console.log('再等 60 帧后画面含 ACCOUNTS? ' + screenText().includes('ACCOUNTS'));
console.log('最终 PC 分布:');
report('最终', samplePC(2, null));
