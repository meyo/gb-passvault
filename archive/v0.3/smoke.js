#!/usr/bin/env node
/*
 * smoke.js - GB Password Vault v0.3 无头冒烟测试 (serverboy)
 *
 * 通过模拟器按键驱动完整流程, 断言:
 *   - 屏幕文本 (读 serverboy 的 DMG tilemap buffer, 等于画面实际内容)
 *   - SRAM 字节 (等于电池存档内容), 含全局校验和 (跨 2 个 8KB 存储体)
 *
 * 关键点 1: ROM 在绘制/延时期间不读按键, 固定帧间隔会丢键。
 *           -> 每个按键都等待画面确认 (字段/高亮格/目标文本) 后才继续, 失败自动重试。
 * 关键点 2: serverboy 每帧结束强制松键, "按住 N 帧"对 waitpad 型界面等于按 N 次。
 *           -> tap = 单帧点按 (一次输入); hold = 连续帧按住 (仅 joypad 轮询界面用)。
 * 关键点 3: 存档为 32KB (MBC1 4 x 8KB 存储体, ROM 以 0x4000/0x6000 寄存器切体),
 *           平面地址 = 体号 * 8192 + 体内偏移, 故槽位 i 的字节 = 存档[16 + i*47]。
 * 注意: serverboy 对 DMG 的 VRAM 建模: tilemap (0x9800) 在 core.BGCHRBank1
 */
const fs = require('fs');
const path = require('path');
const Gameboy = require('serverboy');

const ROOT = path.resolve(__dirname, '..', '..');
const ROM_PATH = path.join(ROOT, 'build', 'passmgr.gb');
const OUT = path.join(ROOT, 'test_out');
fs.mkdirSync(OUT, { recursive: true });

const rom = fs.readFileSync(ROM_PATH);

let gb = null;
let core = null;
let checks = 0;
let fails = 0;

function ok(cond, msg) {
    checks++;
    if (cond) console.log('  ok   ' + msg);
    else { fails++; console.log('  FAIL ' + msg); }
}

function boot(save) {
    gb = new Gameboy();
    gb.loadRom(rom, save);
    const privKey = Object.getOwnPropertyNames(gb).find(k => k.startsWith('_'));
    core = gb[privKey].gameboy;
}

function frames(n) { for (let i = 0; i < n; i++) gb.doFrame(); }
function tap(key) { gb.pressKey(key); gb.doFrame(); }
function hold(key, n) { for (let i = 0; i < n; i++) { gb.pressKey(key); gb.doFrame(); } }
function hold4(keys, n) {
    for (let i = 0; i < n; i++) { for (const k of keys) gb.pressKey(k); gb.doFrame(); }
}

function tileAt(x, y) { return core.BGCHRBank1[(0x9800 + y * 32 + x) & 0x7FF]; }
function mapChar(x, y) { return String.fromCharCode(0x20 + (tileAt(x, y) % 96)); }
function rowText(y) { let s = ''; for (let x = 0; x < 20; x++) s += mapChar(x, y); return s; }
function tilemapText() { const r = []; for (let y = 0; y < 18; y++) r.push(rowText(y)); return r; }
function screenText() { return tilemapText().join('\n'); }

function waitUntil(fn, maxF) {
    maxF = maxF || 400;
    for (let f = 0; f < maxF; f++) {
        if (fn()) return true;
        gb.doFrame();
    }
    return fn();
}
function waitOk(fn, msg, maxF) {
    const got = waitUntil(fn, maxF);
    ok(got, msg);
    return got;
}
function settle(framesStable, holdKeys) {
    framesStable = framesStable || 12;
    let last = null, stable = 0;
    for (let f = 0; f < 300; f++) {
        const snap = tilemapText().join('');
        if (snap === last) { stable++; if (stable >= framesStable) return; }
        else { stable = 0; last = snap; }
        if (holdKeys) hold4(holdKeys, 1); else gb.doFrame();
    }
}
function dump(name, holdKeys) {
    settle(12, holdKeys);
    const rows = tilemapText();
    fs.writeFileSync(path.join(OUT, name + '.txt'), rows.join('\n') + '\n');
    console.log('--- ' + name + ' ---');
    console.log(rows.join('\n').replace(/\s+$/gm, ''));
    return rows;
}
function sram() { return gb.getSaveData(); }
function checksumOk(s) {
    let c = 0xA5;
    for (let i = 0; i < 16; i++) if (i !== 9) c ^= (s[i] & 0xFF);
    for (let i = 16; i < 16 + 255 * 47; i++) c ^= (s[i] & 0xFF);
    return (c & 0xFF) === (s[9] & 0xFF);
}
function recOf(s, i) {
    const b = 16 + i * 47;
    return {
        used: s[b], llen: s[b + 1],
        label: Buffer.from(s.slice(b + 2, b + 10)).toString('latin1'),
        plen: s[b + 10],
        pass: Buffer.from(s.slice(b + 11, b + 47)).toString('latin1')
    };
}
function detailTitle(i) { return 'SLOT ' + String(i + 1).padStart(3, '0') + '/255'; }

/* 点按一次并等待结果, 失败自动重试 (最多 6 次) */
function tapUntil(key, verifyFn, maxWait, retries) {
    maxWait = maxWait || 150; retries = retries || 6;
    for (let t = 0; t < retries; t++) {
        tap(key);
        if (waitUntil(verifyFn, maxWait)) return true;
    }
    return false;
}

/* ============ PIN 数字键盘 ============ */
const PAD_X = [6, 10, 14], PAD_Y = [6, 8, 10, 12];
function padPos(d) { if (d === 0) return { r: 3, c: 1 }; const i = d - 1; return { r: (i / 3) | 0, c: i % 3 }; }
function pinScreenReady(title) {
    return rowText(1).includes(title) && rowText(3).slice(0, 16) === '      PIN:  ____';
}
function pinFieldShown(digits) {
    const f = digits + '_'.repeat(4 - digits.length);
    return rowText(3).slice(12, 16) === f;
}
function padHighlighted(c, r) { return tileAt(PAD_X[c], PAD_Y[r]) >= 96; }

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
            if (!tapUntil(key, () => padHighlighted(hl.c, hl.r), 60, 5)) {
                throw new Error('PIN 光标未移动 (' + key + ' -> ' + hl.c + ',' + hl.r + ')');
            }
        }
        digits.push(pin[i]);
        if (i < pin.length - 1) {
            if (!tapUntil('A', () => pinFieldShown(digits.join('')), 60, 5)) {
                throw new Error('PIN 字段未显示 ' + digits.join(''));
            }
        } else {
            tap('A');   /* 第 4 位触发提交, 由调用方等待下一画面 */
        }
    }
}

/* ============ 虚拟键盘 v2 (三模式: 大写 / 小写 / 符号页) ============ */
const KB_UP = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_@";
const KB_LO = "abcdefghijklmnopqrstuvwxyz0123456789.-_@";
const KB_SYM_ROWS = ["!\"#$%&'(", ")*+,-./:", ";<=>?@[\\", "]^_`{|}~"];
const KX5 = [0, 3, 7, 14, 18];
const MODE_AT_00 = { UPPER: 'A', LOWER: 'a', SYM: '!' };

/* 随机密码生成器字符集 (镜像 ROM: GEN_UP/GEN_LO/GEN_DIG/GEN_SYM) */
const GEN_UP = "ABCDEFGHJKLMNPQRSTUVWXYZ";
const GEN_LO = "abcdefghijkmnopqrstuvwxyz";
const GEN_DIG = "23456789";
const GEN_SYM = ".-_@!#$%&()?+";
const GEN_ALL = GEN_UP + GEN_LO + GEN_DIG + GEN_SYM;

function kbRowMax(mode) { return mode === 'SYM' ? 3 : 4; }
function kbColMax(r) { return r < 5 ? 7 : 4; }
function kbCellX(row, col) { return row < 5 ? 2 + col * 2 : KX5[col]; }
function kbCellY(row) { return 4 + row * 2; }
function kbHighlighted(row, col) { return tileAt(kbCellX(row, col), kbCellY(row)) >= 96; }
function kbCellChar(row, col) {
    return String.fromCharCode(0x20 + (tileAt(kbCellX(row, col), kbCellY(row)) % 96));
}
function kbFieldShown(text) {
    const start = Math.max(0, text.length - 14);
    return rowText(2).slice(1, 16) === (text.slice(start) + '_').padEnd(15, ' ');
}
function kbScreenReady(title) {
    return rowText(1).includes(title) && rowText(2).slice(1, 16) === '_'.padEnd(15, ' ');
}

/* 字符 -> { mode, r, c }; mode='ANY' 表示两字母页同格 (数字/. - _ @/空格) */
function kbGridPos(ch) {
    const cu = ch.charCodeAt(0);
    if (cu >= 65 && cu <= 90) { const i = cu - 65; return { mode: 'UPPER', r: (i / 8) | 0, c: i % 8 }; }
    if (cu >= 97 && cu <= 122) { const i = cu - 97; return { mode: 'LOWER', r: (i / 8) | 0, c: i % 8 }; }
    if (ch === ' ') return { mode: 'ANY', r: 5, c: 2 };
    let i = KB_UP.indexOf(ch);
    if (i >= 0) return { mode: 'ANY', r: (i / 8) | 0, c: i % 8 };
    for (let r = 0; r < 4; r++) {
        const c = KB_SYM_ROWS[r].indexOf(ch);
        if (c >= 0) return { mode: 'SYM', r, c };
    }
    throw new Error('no key for [' + ch + ']');
}

function kbMove(cur, key, r, c) {
    if (!tapUntil(key, () => kbHighlighted(r, c), 120, 6)) {
        throw new Error('键盘光标未移动 ' + key + ' -> (' + r + ',' + c + ')');
    }
    cur.r = r; cur.c = c;
    return cur;
}

/* 移动到底行 col (0=Aa 1=SYM/ABC 2=SPACE 3=DEL 4=OK), 镜像 ROM 的行跳转 */
function kbToBottom(cur, state, col) {
    while (cur.r < 5) {
        const nr = (cur.r === kbRowMax(state.mode)) ? 5 : cur.r + 1;
        const nc = Math.min(cur.c, kbColMax(nr));
        kbMove(cur, 'DOWN', nr, nc);
    }
    while (cur.c > col) kbMove(cur, 'LEFT', 5, cur.c - 1);
    while (cur.c < col) kbMove(cur, 'RIGHT', 5, cur.c + 1);
}

/* 移动到期目标格 (从底行出发时先回到当前模式的最后一行) */
function kbToCell(cur, mode, tr, tc) {
    if (cur.r === 5) {
        const nr = kbRowMax(mode);
        kbMove(cur, 'UP', nr, Math.min(cur.c, kbColMax(nr)));
    }
    while (cur.r > tr) kbMove(cur, 'UP', cur.r - 1, Math.min(cur.c, kbColMax(cur.r - 1)));
    while (cur.r < tr) {
        const nr = (cur.r === kbRowMax(mode)) ? 5 : cur.r + 1;
        kbMove(cur, 'DOWN', nr, Math.min(cur.c, kbColMax(nr)));
    }
    while (cur.c > tc) kbMove(cur, 'LEFT', cur.r, cur.c - 1);
    while (cur.c < tc) kbMove(cur, 'RIGHT', cur.r, cur.c + 1);
}

/* 切换键盘模式 (镜像 ROM: Aa=大小写, SYM/ABC=符号页) */
function kbSwitchTo(cur, state, mode) {
    let guard = 0;
    while (state.mode !== mode) {
        if (++guard > 4) throw new Error('模式切换死循环: ' + state.mode + ' -> ' + mode);
        let keyCol, next;
        if (mode === 'SYM') { keyCol = 1; next = 'SYM'; }
        else if (state.mode === 'SYM') { keyCol = 1; next = 'UPPER'; }
        else if (mode === 'UPPER') { keyCol = 0; next = 'UPPER'; }
        else { keyCol = 0; next = 'LOWER'; }
        kbToBottom(cur, state, keyCol);
        if (!tapUntil('A', () => kbCellChar(0, 0) === MODE_AT_00[next], 200, 6)) {
            throw new Error('模式未切到 ' + next + ' (当前 ' + state.mode + ')');
        }
        state.mode = next;
    }
}

function typeText(title, text, doneVerify) {
    if (!waitUntil(() => kbScreenReady(title), 400)) throw new Error('未进入 ' + title + ' 输入画面');
    const cur = { r: 0, c: 0 };
    const state = { mode: 'UPPER' };
    let acc = '';
    for (const ch of text) {
        const t = kbGridPos(ch);
        if (t.mode !== 'ANY' && t.mode !== state.mode) kbSwitchTo(cur, state, t.mode);
        kbToCell(cur, state.mode, t.r, t.c);
        acc += ch;
        if (!tapUntil('A', () => kbFieldShown(acc), 120, 6)) throw new Error('字段未显示 [' + acc + ']');
    }
    if (doneVerify) {
        if (!tapUntil('START', doneVerify, 200, 6)) throw new Error(title + ' 按 START 后未进入下一画面');
    } else {
        tap('START');
    }
}

/* ============ 菜单 / 列表 ============ */
function menuSel() { for (let i = 0; i < 4; i++) if (tileAt(2, 6 + i * 2) >= 96) return i; return -1; }
function listSel() { for (let i = 0; i < 12; i++) if (tileAt(1, 3 + i) >= 96) return i; return -1; }
/* 列表页真正就绪: 标题 + nnn/255 位置 + 窗口首末行已绘制
 * (菜单 "VIEW ACCOUNTS" 也含 ACCOUNTS, 必须用位置串区分) */
function listReady() {
    return rowText(0).includes('ACCOUNTS') && /\d\d\d\/255/.test(rowText(0))
        && rowText(3).trim() !== '' && rowText(14).trim() !== '';
}
function menuGoto(i) {
    if (!waitUntil(() => menuSel() >= 0, 500)) throw new Error('菜单高亮未找到');
    let cur = menuSel();
    while (cur < i) {
        if (!tapUntil('DOWN', () => menuSel() === cur + 1, 60, 5)) throw new Error('菜单未下移');
        cur++;
    }
    while (cur > i) {
        if (!tapUntil('UP', () => menuSel() === cur - 1, 60, 5)) throw new Error('菜单未上移');
        cur--;
    }
}
function listGoto(i) {
    if (!waitUntil(() => listSel() >= 0, 500)) throw new Error('列表高亮未找到');
    let cur = listSel();
    while (cur < i) {
        if (!tapUntil('DOWN', () => listSel() === cur + 1, 60, 5)) throw new Error('列表未下移');
        cur++;
    }
    while (cur > i) {
        if (!tapUntil('UP', () => listSel() === cur - 1, 60, 5)) throw new Error('列表未上移');
        cur--;
    }
}

/* 合成 32KB 存档: PIN 1234 + count 个账号 (SLOT01..n / P01..n), 含校验和 */
function buildSyntheticSave(count) {
    const s = Buffer.alloc(32768);
    s.write('PMG3', 0, 'latin1');
    s[4] = 8; s[5] = 9; s[6] = 0; s[7] = 1;    /* (1234 + 7) % 10 */
    s[8] = 0; s[10] = 1;                        /* tries = 0, PIN 已设置 */
    for (let i = 0; i < count; i++) {
        const b = 16 + i * 47;
        const label = 'SLOT' + String(i + 1).padStart(2, '0');
        const pass = 'P' + String(i + 1).padStart(2, '0');
        s[b] = 0xA5;
        s[b + 1] = label.length;
        s.write(label, b + 2, 'latin1');
        s[b + 10] = pass.length;
        s.write(pass, b + 11, 'latin1');
    }
    let c = 0xA5;
    for (let i = 0; i < 16; i++) if (i !== 9) c ^= s[i];
    for (let i = 16; i < 16 + 255 * 47; i++) c ^= s[i];
    s[9] = c & 0xFF;
    return s;
}

/* ================================================================== */

try {
    console.log('== 流程 A: 全新卡带 (无存档) ==');
    boot();
    waitOk(() => screenText().includes('PRESS START'), '标题画面出现 PRESS START');
    dump('01-title');
    ok(screenText().includes('GB PASSWORD'), '标题文字 GB PASSWORD');
    ok(screenText().includes('V0.3'), '标题版本 V0.3');

    if (!tapUntil('START', () => pinScreenReady('NEW PIN'), 300, 8)) throw new Error('无法进入设置 PIN 画面');
    ok(true, '进入设置 PIN 画面');
    dump('02-newpin');

    enterPin('1234');
    waitOk(() => pinScreenReady('CONFIRM PIN'), '进入确认 PIN 画面');
    enterPin('1234');
    waitOk(() => screenText().includes('VAULT MENU'), '设置完成进入主菜单');
    dump('03-menu');

    let s = sram();
    ok(rom[0x149] === 0x03, 'ROM 头声明 32KB SRAM (0x149 = 0x03)');
    ok(s.length === 32768, 'SRAM 快照 = 32768 字节 (4 x 8KB 存储体)');
    ok(String.fromCharCode(s[0], s[1], s[2], s[3]) === 'PMG3', 'SRAM MAGIC = PMG3');
    ok(s[4] === 8 && s[5] === 9 && s[6] === 0 && s[7] === 1, 'PIN 1234 以 +7 混淆存储 (8 9 0 1)');
    ok(s[8] === 0, '错误次数 = 0');
    ok((s[10] & 1) === 1, '已设置 PIN 标志 = 1');
    ok(checksumOk(s), 'SRAM 全局校验和正确 (16 + 255*47 字节)');

    /* 菜单 -> ADD ACCOUNT: 自动分配第一个空槽 -> 标签/密码 */
    menuGoto(1);
    if (!tapUntil('A', () => kbScreenReady('LABEL (MAX 8)'), 200, 6)) throw new Error('ADD ACCOUNT 未进入标签输入');
    ok(true, 'ADD ACCOUNT 直接进入标签输入 (自动分配空槽)');
    typeText('LABEL (MAX 8)', 'EMAIL', () => kbScreenReady('PASSWORD (MAX 36)'));
    ok(true, '标签 EMAIL 输入完成, 进入密码输入画面');
    typeText('PASSWORD (MAX 36)', 'ABC', () => screenText().includes('VAULT MENU'));
    ok(true, '密码 ABC 输入完成, 保存并回到主菜单');
    dump('05-menu-after-save');

    s = sram();
    let r0 = recOf(s, 0);
    ok(r0.used === 0xA5, '槽位 1 used = 0xA5');
    ok(r0.llen === 5 && r0.label.startsWith('EMAIL'), '槽位 1 标签 = EMAIL');
    ok(r0.plen === 3 && r0.pass.startsWith('ABC'), '槽位 1 密码 = ABC');
    ok(checksumOk(s), '写入槽位后 SRAM 校验和正确');

    /* 第二账号: 36 位混合大小写 + 符号密码 (键盘 v2 三模式) */
    menuGoto(1);
    if (!tapUntil('A', () => kbScreenReady('LABEL (MAX 8)'), 200, 6)) throw new Error('未进入标签输入 (第二账号)');
    typeText('LABEL (MAX 8)', 'MIXED', () => kbScreenReady('PASSWORD (MAX 36)'));
    ok(true, '标签 MIXED 输入完成 (键盘 v2)');
    const LONG = 'aB3!xY9#qW2$eR5%tY8&uI1(uO4)Kz6_@b?!';
    typeText('PASSWORD (MAX 36)', LONG, () => screenText().includes('VAULT MENU'));
    ok(true, '36 位混合大小写 + 符号密码输入完成并保存');
    dump('05b-menu-after-mixed');

    s = sram();
    const r1 = recOf(s, 1);
    ok(r1.used === 0xA5 && r1.llen === 5 && r1.label.startsWith('MIXED'), '槽位 2 标签 = MIXED');
    ok(r1.plen === 36 && r1.pass === LONG, '槽位 2 密码 = 36 位混合大小写 + 符号');
    ok(checksumOk(s), '写入长密码后 SRAM 校验和正确');

    /* 第三账号: 随机密码生成器 (SELECT 生成 + 重掷 + 四类字符) */
    menuGoto(1);
    if (!tapUntil('A', () => kbScreenReady('LABEL (MAX 8)'), 200, 6)) throw new Error('未进入标签输入 (第三账号)');
    typeText('LABEL (MAX 8)', 'GENACC', () => kbScreenReady('PASSWORD (MAX 36)'));
    ok(true, '标签 GENACC 输入完成 (第三账号)');
    ok(rowText(2).slice(1, 16) === '_'.padEnd(15, ' '), '密码页初始为空 (仅光标)');

    const genFieldOk = () => rowText(2).slice(2, 16).trim() !== '';   /* 跳过 x=1 光标 '_' */
    if (!tapUntil('SELECT', genFieldOk, 200, 6)) throw new Error('SELECT 未生成随机密码');
    ok(true, '密码页按 SELECT 生成随机密码');
    dump('05c-generated-password');
    const genW1 = rowText(2);
    let rerolled = false;
    for (let t = 0; t < 6 && !rerolled; t++) {
        tap('SELECT');
        rerolled = waitUntil(() => rowText(2) !== genW1, 120);
    }
    ok(rerolled, '再按 SELECT 重掷 (内容变化)');
    const shownTail = rowText(2).slice(1, 15);   /* 窗内前 14 字符 = 密码第 3..16 位 */

    frames(5);
    tap('START');
    waitOk(() => screenText().includes('VAULT MENU'), '生成密码保存后回到主菜单', 300);

    s = sram();
    const r2 = recOf(s, 2);
    ok(r2.used === 0xA5 && r2.llen === 6 && r2.label.startsWith('GENACC'), '槽位 3 标签 = GENACC');
    ok(r2.plen === 16, '生成密码长度 = 16');
    const pass2 = r2.pass.slice(0, 16);
    ok([...pass2].every(c => GEN_ALL.includes(c)), '生成密码字符全部来自安全字符集 (无 0O1lI)');
    ok(/[A-Z]/.test(pass2) && /[a-z]/.test(pass2) && /[2-9]/.test(pass2) && /[.\-_@!#$%&()?+]/.test(pass2),
        '生成密码含大写/小写/数字/符号四类字符');
    ok(pass2.slice(2, 16) === shownTail, '保存的密码与画面显示尾部一致');
    ok(checksumOk(s), '生成密码写入后 SRAM 校验和正确');

    /* 列表 -> 详情 -> 显示密码 -> 返回 -> 锁库 */
    menuGoto(0);
    if (!tapUntil('A', () => listReady(), 200, 6)) throw new Error('未进入账号列表');
    ok(true, '进入账号列表');
    dump('06-list');
    ok(rowText(0).includes('001/255'), '列表右上角显示 001/255');
    ok(screenText().includes('001 EMAIL'), '列表第 1 行显示 001 EMAIL');
    ok(screenText().includes('002 MIXED'), '列表第 2 行显示 002 MIXED');
    ok(screenText().includes('003 GENACC'), '列表第 3 行显示 003 GENACC');

    listGoto(0);
    if (!tapUntil('A', () => screenText().includes(detailTitle(0)), 200, 6)) throw new Error('未进入详情页');
    ok(true, '进入详情页 (SLOT 001/255)');
    dump('07-detail-masked');
    ok(rowText(5).includes('***'), '密码默认打码 ***');

    hold('A', 40);
    dump('08-detail-reveal', ['A']);
    ok(rowText(5).includes('ABC'), '按住 A 显示明文 ABC');
    frames(10);

    if (!tapUntil('B', () => listReady(), 200, 6)) throw new Error('未能返回列表');
    ok(true, '详情页返回列表');

    /* MIXED 长密码详情: 打码 20+16, 按住 A 两行明文 */
    listGoto(1);
    if (!tapUntil('A', () => screenText().includes(detailTitle(1)), 200, 6)) throw new Error('未进入 MIXED 详情页');
    dump('07b-detail-long-masked');
    const stars5 = (rowText(5).match(/\*/g) || []).length;
    const stars6 = (rowText(6).match(/\*/g) || []).length;
    ok(stars5 === 20 && stars6 === 16, '36 位密码打码 (行5=20颗 行6=16颗)');
    hold('A', 40);
    dump('08b-detail-long-reveal', ['A']);
    ok((rowText(5) + rowText(6)).trimEnd() === LONG, '按住 A 显示 36 位明文 (两行)');
    frames(10);
    if (!tapUntil('B', () => listReady(), 200, 6)) throw new Error('未能从长密码详情返回列表');
    ok(true, '长密码详情返回列表');

    if (!tapUntil('B', () => screenText().includes('VAULT MENU'), 200, 6)) throw new Error('未能返回菜单');
    ok(true, '列表返回主菜单');
    menuGoto(3);
    if (!tapUntil('A', () => pinScreenReady('ENTER PIN'), 200, 6)) throw new Error('未能进入解锁画面');
    ok(true, '锁库后进入解锁画面');
    dump('09-locked-to-unlock');

    const save1 = sram();
    fs.writeFileSync(path.join(OUT, 'save1.bin'), Buffer.from(save1));
    console.log('已保存 SRAM 快照 save1.bin (' + save1.length + ' 字节)');

    console.log();
    console.log('== 流程 B: 错误 PIN 三次 -> SYSTEM LOCKED -> 擦除恢复 ==');
    boot(save1);
    if (!tapUntil('START', () => pinScreenReady('ENTER PIN'), 300, 8)) throw new Error('未进入解锁画面');
    ok(true, '重载存档后进入解锁画面');
    dump('10-unlock');

    for (let i = 0; i < 3; i++) {
        enterPin('0000');
        if (i < 2) waitOk(() => pinScreenReady('ENTER PIN'), '第 ' + (i + 1) + ' 次错误后回到解锁画面');
    }
    waitOk(() => screenText().includes('SYSTEM LOCKED'), '错误 3 次进入 SYSTEM LOCKED 彩蛋', 900);
    dump('11-system-locked');
    s = sram();
    ok(s[8] === 3, '错误次数持久化 = 3');

    /* 按住 A+B+SELECT+START 三秒恢复 (joypad 轮询 + 硬件定时器计时) */
    const COMBO = ['A', 'B', 'SELECT', 'START'];
    hold4(COMBO, 100);   /* ≈1.2 秒: 不应触发擦除 */
    ok(!screenText().includes('NEW PIN'), '长按不足 3 秒不触发擦除');
    let recovered = false;
    for (let f = 0; f < 1200 && !recovered; f++) {
        hold4(COMBO, 1);
        recovered = screenText().includes('NEW PIN');
    }
    ok(recovered, '长按 3 秒后擦除, 恢复首次设置流程');
    frames(5);           /* 松开按键, 等画面稳定 */
    waitOk(() => pinScreenReady('NEW PIN'), '擦除后稳定在设置 PIN 画面', 300);
    dump('12-after-recovery');
    s = sram();
    ok((s[10] & 1) === 0, '擦除后 PIN 标志 = 0');
    ok(s[8] === 0, '擦除后错误次数 = 0');
    ok(recOf(s, 0).used === 0, '擦除后槽位 1 为空');
    ok(recOf(s, 1).used === 0, '擦除后槽位 2 为空');
    ok(recOf(s, 2).used === 0, '擦除后槽位 3 为空');
    ok(checksumOk(s), '擦除后 SRAM 校验和正确');
    ok(String.fromCharCode(s[0], s[1], s[2], s[3]) === 'PMG3', '擦除后 MAGIC 仍在');

    console.log();
    console.log('== 流程 C: 用 save1 重新上电 -> 正确 PIN -> 数据仍在 ==');
    boot(save1);
    if (!tapUntil('START', () => pinScreenReady('ENTER PIN'), 300, 8)) throw new Error('未进入解锁画面');
    ok(true, '重新上电进入解锁画面');
    enterPin('1234');
    waitOk(() => screenText().includes('VAULT MENU'), '正确 PIN 解锁进入主菜单');
    dump('13-menu-after-unlock');
    menuGoto(0);
    if (!tapUntil('A', () => listReady(), 200, 6)) throw new Error('未进入账号列表');
    ok(true, '进入账号列表');
    dump('14-list-persisted');
    ok(screenText().includes('001 EMAIL'), '断电重开后账号 EMAIL 仍在');
    ok(screenText().includes('003 GENACC'), '断电重开后账号 GENACC 仍在');
    listGoto(0);
    if (!tapUntil('A', () => screenText().includes(detailTitle(0)), 200, 6)) throw new Error('未进入详情页');
    hold('A', 40);
    dump('15-reveal-persisted', ['A']);
    ok(rowText(5).includes('ABC'), '断电重开后密码 ABC 仍在');

    console.log();
    console.log('== 流程 D: 255 槽滚动列表 + 末槽写入/删除 (合成 20 账号存档) ==');
    boot(buildSyntheticSave(20));
    if (!tapUntil('START', () => pinScreenReady('ENTER PIN'), 300, 8)) throw new Error('流程 D: 未进入解锁画面');
    enterPin('1234');
    waitOk(() => screenText().includes('VAULT MENU'), '流程 D: 解锁进入主菜单');
    menuGoto(0);
    if (!tapUntil('A', () => listReady(), 200, 6)) throw new Error('流程 D: 未进入账号列表');
    ok(rowText(0).includes('001/255'), '列表右上角显示 001/255');
    ok(rowText(3).includes('001 SLOT01'), '窗口首行 = 001 SLOT01');
    ok(rowText(14).includes('012 SLOT12'), '窗口末行 = 012 SLOT12 (12 行窗口)');
    ok(!screenText().includes('SLOT13'), 'SLOT13 不在初始窗口');
    dump('16-list-window-top');

    if (!tapUntil('DOWN', () => rowText(0).includes('002/255') && listSel() === 1, 150, 3)) throw new Error('流程 D: 列表下移失败');
    ok(listSel() === 1, '下移后高亮在第 2 行');
    ok(rowText(3).includes('001 SLOT01'), '窗口未滚动 (仍从 SLOT01 开始)');

    /* 右跳 8: 2 -> 10 (仍在窗口内, 不滚动) */
    if (!tapUntil('RIGHT', () => rowText(0).includes('010/255') && listSel() === 9 && rowText(12).includes('010 SLOT10'), 150, 3)) throw new Error('流程 D: 右跳失败');
    ok(listSel() === 9, '右跳后高亮 = 第 10 条 (窗口内)');
    ok(rowText(3).includes('001 SLOT01'), '窗口仍未滚动 (9 < 12 行窗口)');
    ok(rowText(12).includes('010 SLOT10'), '高亮行 = 010 SLOT10');

    /* 再右跳 8: 10 -> 18 (超出窗口, 滚动) */
    if (!tapUntil('RIGHT', () => rowText(0).includes('018/255') && listSel() === 11 && rowText(3).includes('007 SLOT07'), 150, 3)) throw new Error('流程 D: 右跳 2 失败');
    ok(listSel() === 11, '再右跳后高亮位于窗口末行');
    ok(rowText(3).includes('007 SLOT07'), '窗口滚动后首行 = 007 SLOT07');
    ok(rowText(14).includes('018 SLOT18'), '高亮行 = 018 SLOT18');

    /* 左跳 8: 18 -> 10 */
    if (!tapUntil('LEFT', () => rowText(0).includes('010/255') && listSel() === 3 && rowText(6).includes('010 SLOT10'), 150, 3)) throw new Error('流程 D: 左跳失败');
    ok(listSel() === 3, '左跳后高亮在窗口第 4 行');
    ok(rowText(6).includes('010 SLOT10'), '高亮行 = 010 SLOT10');

    /* 上移 2 格后打开第 8 号槽详情 */
    if (!tapUntil('UP', () => listSel() === 2, 150, 3)) throw new Error('流程 D: 上移失败');
    if (!tapUntil('UP', () => listSel() === 1, 150, 3)) throw new Error('流程 D: 上移 2 失败');
    if (!tapUntil('A', () => screenText().includes(detailTitle(7)), 200, 6)) throw new Error('流程 D: 未打开 SLOT08 详情');
    ok(true, '打开第 8 号槽位详情 (SLOT 008/255)');
    hold('A', 40);
    ok(rowText(5).includes('P08'), '按住 A 显示明文 P08');
    frames(10);
    if (!tapUntil('B', () => listReady(), 200, 6)) throw new Error('流程 D: 详情返回失败');
    ok(listSel() === 1, '返回列表后高亮位置保持');

    /* 连续右跳直达底部 255: 每次目标精确校验 (丢帧的点按自动重发, 不会重跳) */
    let selJ = 7, viewJ = 6;   /* 镜像 ROM 的当前选择/窗口状态 */
    let bottomOk = true;
    while (selJ < 254) {
        selJ = Math.min(selJ + 8, 254);
        if (selJ < viewJ) viewJ = selJ;
        else if (selJ >= viewJ + 12) viewJ = selJ + 1 - 12;
        const titleExp = String(selJ + 1).padStart(3, '0') + '/255';
        const rowExp = 3 + (selJ - viewJ);
        const verify = () => rowText(0).includes(titleExp) && tileAt(1, rowExp) >= 96
            && rowText(3).trim() !== '' && rowText(14).trim() !== '';
        if (!tapUntil('RIGHT', verify, 150, 3)) { bottomOk = false; break; }
    }
    ok(bottomOk && rowText(0).includes('255/255'), '连续右跳可直达底部 255/255');
    ok(listSel() === 11, '底部高亮位于窗口末行');
    ok(rowText(14).includes('255 EMPTY'), '末行 = 255 EMPTY');
    dump('17-list-window-bottom');
    tap('RIGHT');
    waitUntil(() => rowText(0).includes('255/255') && rowText(14).includes('255 EMPTY'), 60);
    ok(rowText(0).includes('255/255'), '底部再右跳保持 255/255');

    if (!tapUntil('A', () => kbScreenReady('LABEL (MAX 8)'), 200, 6)) throw new Error('流程 D: 空槽 A 未进入标签输入');
    ok(true, '在 255 号空槽按 A 直接进入添加流程');
    dump('18-add-at-255');
    if (!tapUntil('SELECT', () => listReady(), 200, 6)) throw new Error('流程 D: 标签页 SELECT 未取消');
    ok(rowText(0).includes('255/255'), '取消添加后回到列表且位置保持');

    if (!tapUntil('A', () => kbScreenReady('LABEL (MAX 8)'), 200, 6)) throw new Error('流程 D: 二次进入标签输入失败');
    typeText('LABEL (MAX 8)', 'LAST', () => kbScreenReady('PASSWORD (MAX 36)'));
    if (!tapUntil('SELECT', () => rowText(2).slice(2, 16).trim() !== '', 200, 6)) throw new Error('流程 D: 255 号槽密码生成失败');
    const tail254 = rowText(2).slice(1, 15);
    frames(5);
    tap('START');
    waitOk(() => listReady(), '保存后回到列表 (255 号槽)', 300);
    ok(rowText(0).includes('255/255'), '位置仍是 255/255');
    ok(rowText(14).includes('255 LAST'), '行文本更新为 255 LAST');

    s = sram();
    const r254 = recOf(s, 254);
    ok(r254.used === 0xA5 && r254.llen === 4 && r254.label.startsWith('LAST'), '第 255 槽标签 = LAST');
    ok(r254.plen === 16 && r254.pass.slice(2, 16) === tail254, '第 255 槽密码 = 生成的 16 位 (与显示一致)');
    ok(checksumOk(s), '跨存储体地址空间 (12001 字节) 校验和正确');

    /* 详情删除 255 号槽 */
    if (!tapUntil('A', () => screenText().includes(detailTitle(254)), 200, 6)) throw new Error('流程 D: 未打开 SLOT 255 详情');
    if (!tapUntil('START', () => screenText().includes('DELETE THIS SLOT?'), 200, 6)) throw new Error('流程 D: 未出现删除确认');
    ok(true, '详情页 START 弹出删除确认');
    if (!tapUntil('A', () => listReady(), 400, 6)) throw new Error('流程 D: 删除后未返回列表');
    ok(rowText(14).includes('255 EMPTY'), '删除后 255 号槽恢复 EMPTY');
    s = sram();
    ok(recOf(s, 254).used === 0, '删除后 255 号槽 used = 0');
    ok(checksumOk(s), '删除后 SRAM 校验和正确');
} catch (e) {
    fails++;
    console.log();
    console.log('*** 测试中断: ' + e.message + ' ***');
    dump('99-crash-screen');
}

console.log();
console.log('==============================');
console.log('断言: ' + Math.max(checks - fails, 0) + ' / ' + checks + ' 通过');
if (fails) { console.log('*** ' + fails + ' 个失败 ***'); process.exit(1); }
console.log('全部通过');
