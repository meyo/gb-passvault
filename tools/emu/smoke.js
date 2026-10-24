#!/usr/bin/env node
/*
 * smoke.js - GB Password Vault v0.3.1 无头冒烟测试 (serverboy)
 *
 * 通过模拟器按键驱动完整流程, 断言:
 *   - 屏幕文本 (读 serverboy 的 DMG tilemap buffer, 等于画面实际内容)
 *   - SRAM 字节 (等于电池存档内容), 含全局校验和 (PMG4, 跨 3 个 8KB 存储体)
 *   - v0.3 (PMG3) 存档的一次性迁移
 *
 * 关键点 1: ROM 在绘制/延时期间不读按键, 固定帧间隔会丢键。
 *           -> 每个按键都等待画面确认 (字段/高亮格/目标文本) 后才继续, 失败自动重试。
 * 关键点 2: serverboy 每帧结束强制松键, "按住 N 帧"对 waitpad 型界面等于按 N 次。
 *           -> tap = 单帧点按 (一次输入); hold = 连续帧按住 (仅 joypad 轮询界面用)。
 * 关键点 3: 存档为 32KB (MBC1 4 x 8KB 存储体), 平面地址 = 体号 * 8192 + 体内偏移。
 *           v0.3.1 记录: 16 + i*80, 头 16B, 总 20416B。
 * 注意: serverboy 对 DMG 的 VRAM 建模: tilemap (0x9800) 在 core.BGCHRBank1
 */
const fs = require('fs');
const path = require('path');
const Gameboy = require('serverboy');

const ROOT = path.resolve(__dirname, '..', '..');
const ROM_PATH = path.join(ROOT, 'build', 'passmgr.gb');
const OUT = path.join(ROOT, 'test_out');
const V03_SAVE = path.join(__dirname, 'fixtures', 'v03-save.bin');
fs.mkdirSync(OUT, { recursive: true });

const rom = fs.readFileSync(ROM_PATH);

const REC_SZ = 80;
const VAULT_BYTES = 16 + 255 * REC_SZ;   /* 20416 */
const MIG_TMP = 32768 - 255 * REC_SZ;    /* 12368 */

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
function chord(keys) { for (const k of keys) gb.pressKey(k); gb.doFrame(); }

function tileAt(x, y) { return core.BGCHRBank1[(0x9800 + y * 32 + x) & 0x7FF]; }
const TITLE_CHARS = ' SECRTVAUL';     /* tile 238..247: space, S,E,C,R,T,V,A,U,L */
const LABEL_CHARS = '0123456789CLROK'; /* tile 223..237 */
function mapChar(x, y) {
    const t = tileAt(x, y);
    if (t >= 238 && t <= 247) return TITLE_CHARS[t - 238];
    if (t >= 223 && t <= 237) return LABEL_CHARS[t - 223];
    if (t >= 201 && t <= 211) {
        if (t === 211) return '_';
        return String.fromCharCode(48 + (t - 201));
    }
    if (t >= 212 && t <= 222) return ' ';
    if (t >= 192 && t <= 200) return ' ';
    return String.fromCharCode(0x20 + (t % 96));
}
function rowText(y) { let s = ''; for (let x = 0; x < 20; x++) s += mapChar(x, y); return s; }
function tilemapText() { const r = []; for (let y = 0; y < 18; y++) r.push(rowText(y)); return r; }
function screenText() { return tilemapText().join('\n'); }
function rowWindow(y) { return rowText(y).slice(5, 19); }   /* "001 " 之后的 14 字符 */

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
    /* 同时留一份 PPM，便于用 ffmpeg 转 PNG 做视觉验收（.gitignore 默认忽略） */
    const scr = gb.getScreen();
    const ppm = Buffer.alloc(160 * 144 * 3);
    for (let i = 0, p = 0; i < scr.length; i += 4, p += 3) {
        ppm[p] = scr[i]; ppm[p + 1] = scr[i + 1]; ppm[p + 2] = scr[i + 2];
    }
    fs.writeFileSync(path.join(OUT, name + '.ppm'),
        Buffer.concat([Buffer.from('P6\n160 144\n255\n', 'ascii'), ppm]));
    console.log('--- ' + name + ' ---');
    console.log(rows.join('\n').replace(/\s+$/gm, ''));
    return rows;
}
function sram() { return gb.getSaveData(); }
function checksumOk(s) {
    let c = 0xA5;
    for (let i = 0; i < 16; i++) if (i !== 11) c ^= (s[i] & 0xFF);
    for (let i = 16; i < VAULT_BYTES; i++) c ^= (s[i] & 0xFF);
    return (c & 0xFF) === (s[11] & 0xFF);
}
function legacyChecksumOk(s) {
    let c = 0xA5;
    for (let i = 0; i < 16; i++) if (i !== 9) c ^= (s[i] & 0xFF);
    for (let i = 16; i < 12001; i++) c ^= (s[i] & 0xFF);
    return (c & 0xFF) === (s[9] & 0xFF);
}
function recOf(s, i) {
    const b = 16 + i * REC_SZ;
    return {
        used: s[b], llen: s[b + 1],
        label: Buffer.from(s.slice(b + 2, b + 22)).toString('latin1'),
        ulen: s[b + 22],
        user: Buffer.from(s.slice(b + 23, b + 43)).toString('latin1'),
        plen: s[b + 43],
        pass: Buffer.from(s.slice(b + 44, b + 80)).toString('latin1')
    };
}
function legacyRecOf(s, i) {
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

/* ============ PIN 数字键盘 (6 位; 迁移旧档 4 位) ============ */
const PAD_X = [4, 10, 16], PAD_Y = [6, 9, 12, 15];
function padPos(d) { if (d === 0) return { r: 3, c: 1 }; const i = d - 1; return { r: (i / 3) | 0, c: i % 3 }; }
function pinFieldStart(width) { return (20 - width * 3) >> 1; }
function pinFieldShown(digits, width) {
    width = width || 6;
    const start = pinFieldStart(width);
    for (let i = 0; i < width; i++) {
        const x = start + i * 3;
        let center;
        if (i < digits.length) {
            const d = digits.charCodeAt(i) - 48;
            center = 201 + d;
        } else if (i === digits.length) {
            center = 211;
        } else {
            center = 196;
        }
        if (tileAt(x, 3) !== 195 || tileAt(x + 1, 3) !== center || tileAt(x + 2, 3) !== 197) return false;
    }
    return true;
}
function pinScreenReady(title, width) {
    width = width || 6;
    return rowText(0).includes('SECRET VAULT')
        && rowText(1).includes(title)
        && tileAt(0, 0) === 238
        && pinFieldShown('', width)
        && rowText(15).includes('CLR')
        && rowText(15).includes('OK');
}
function padHighlighted(c, r) {
    const t = tileAt(PAD_X[c], PAD_Y[r]);
    return t >= 96 && t < 192;
}

function enterPin(pin) {
    const width = pin.length;
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
            if (!tapUntil('A', () => pinFieldShown(digits.join(''), width), 60, 5)) {
                throw new Error('PIN 字段未显示 ' + digits.join(''));
            }
        } else {
            /* 最后一击可能被绘制/延时吞掉: 屏幕有任何变化即视为已提交 */
            for (let t = 0; t < 4; t++) {
                const before = screenText();
                tap('A');
                if (waitUntil(() => screenText() !== before, 120)) break;
            }
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
const GEN_ALL_NS = GEN_UP + GEN_LO + GEN_DIG;

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
function kbScreenWith(title, text) {
    return rowText(1).includes(title) && kbFieldShown(text);
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

/* 在预填内容的编辑页追加文本 (光标从左上角开始) */
function typeMore(title, initial, text, doneVerify) {
    if (!waitUntil(() => kbScreenWith(title, initial), 400)) throw new Error('未进入编辑画面 ' + title);
    const cur = { r: 0, c: 0 };
    const state = { mode: 'UPPER' };
    let acc = initial;
    for (const ch of text) {
        const t = kbGridPos(ch);
        if (t.mode !== 'ANY' && t.mode !== state.mode) kbSwitchTo(cur, state, t.mode);
        kbToCell(cur, state.mode, t.r, t.c);
        acc += ch;
        if (!tapUntil('A', () => kbFieldShown(acc), 120, 6)) throw new Error('编辑字段未显示 [' + acc + ']');
    }
    if (doneVerify) {
        if (!tapUntil('START', doneVerify, 200, 6)) throw new Error(title + ' 按 START 后未进入下一画面');
    } else {
        tap('START');
    }
}

/* ============ 菜单 / 列表 ============ */
function menuSel() { for (let i = 0; i < 6; i++) if (tileAt(2, 3 + i * 2) >= 96) return i; return -1; }
function settingsSel() { for (let i = 0; i < 3; i++) if (tileAt(1, 4 + i * 2) >= 96) return i; return -1; }
function binChecksumOk(s) {
    if (String.fromCharCode(s[20416], s[20417], s[20418], s[20419]) !== 'BIN1') return false;
    let c = 0xA5 ^ s[20420];
    for (let i = 0; i < 16 * REC_SZ; i++) c ^= s[20432 + i];
    return (c & 0xFF) === s[20421];
}
function listSel() { for (let i = 0; i < 12; i++) if (tileAt(1, 3 + i) >= 96) return i; return -1; }
/* 列表页真正就绪: 标题 + nnn/255 位置 + 窗口首末行已绘制
 * (菜单 "VIEW ACCOUNTS" 也含 ACCOUNTS, 必须用位置串区分) */
function listReady() {
    return rowText(0).includes('ACCOUNTS') && /\d\d\d\/255/.test(rowText(0))
        && rowText(3).trim() !== '' && rowText(14).trim() !== ''
        && rowText(16).includes('SEL=');
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

/* 合成 32KB v0.3.1 存档: PIN 123456 + count 个账号 (SLOTnn / USERnn / Pnn) */
function buildSyntheticSave(count) {
    const s = Buffer.alloc(32768);
    s.write('PMG4', 0, 'latin1');
    s[4] = 8; s[5] = 9; s[6] = 0; s[7] = 1; s[8] = 2; s[9] = 3;   /* 123456 */
    s[10] = 0; s[12] = 1;                                        /* tries = 0, PIN 已设置 */
    for (let i = 0; i < count; i++) {
        const b = 16 + i * REC_SZ;
        const label = 'SLOT' + String(i + 1).padStart(2, '0');
        const user = 'USER' + String(i + 1).padStart(2, '0');
        const pass = 'P' + String(i + 1).padStart(2, '0');
        s[b] = 0xA5;
        s[b + 1] = label.length;
        s.write(label, b + 2, 'latin1');
        s[b + 22] = user.length;
        s.write(user, b + 23, 'latin1');
        s[b + 43] = pass.length;
        s.write(pass, b + 44, 'latin1');
    }
    let c = 0xA5;
    for (let i = 0; i < 16; i++) if (i !== 11) c ^= s[i];
    for (let i = 16; i < VAULT_BYTES; i++) c ^= s[i];
    s[11] = c & 0xFF;
    return s;
}

/* ================================================================== */

try {
    console.log('== 流程 0: 损坏/未知存档保护 (绝不静默清空) ==');
    {
        const good = buildSyntheticSave(2);
        const bad = Buffer.from(good);
        bad[11] ^= 0xFF;                         /* 破坏 PMG4 全局校验和 */
        boot(bad);
        if (!waitUntil(() => screenText().includes('PRESS START'), 400)) throw new Error('损坏档: 标题未出现');
        tap('START');
        if (!waitUntil(() => screenText().includes('SAVE CORRUPT'), 400)) throw new Error('损坏档: 未显示 SAVE CORRUPT');
        if (!waitUntil(() => rowText(6).includes('SCAN FINDS'), 300)) throw new Error('损坏档: 扫描行未绘制完成');
        frames(3);
        ok(!screenText().includes('FIRST TIME SETUP'), '损坏档未进入首次设置 (未静默清空)');
        ok(screenText().includes('DATA IS UNTOUCHED'), '损坏档明确提示数据未改动');
        ok(rowText(6).includes('002/255'), '只读扫描发现 2 条可识别记录');
        dump('00-corrupt');
        const before = Buffer.from(sram());
        frames(120);
        ok(Buffer.compare(before, Buffer.from(sram())) === 0, '停留 120 帧未写 SRAM');
        if (!tapUntil('A', () => screenText().includes('SAVE CORRUPT'), 200, 4)) throw new Error('损坏档: RETRY 未重新检测');
        ok(Buffer.compare(before, Buffer.from(sram())) === 0, 'RETRY 未写 SRAM');
        if (!tapUntil('START', () => screenText().includes('ERASE ALL DATA?'), 200, 4)) throw new Error('损坏档: 未进入擦除确认');
        ok(Buffer.compare(before, Buffer.from(sram())) === 0, '擦除确认前未写 SRAM');
        if (!tapUntil('A', () => pinScreenReady('NEW PIN', 6), 600, 6)) throw new Error('损坏档: 确认擦除后未进入设置');
        const wiped = sram();
        ok(String.fromCharCode(wiped[0], wiped[1], wiped[2], wiped[3]) === 'PMG4', '显式擦除后重建 PMG4 头');
        ok(recOf(wiped, 0).used === 0 && recOf(wiped, 1).used === 0, '显式擦除后槽位清零');
        ok(checksumOk(wiped), '显式擦除后校验和有效');
    }
    {
        const unknown = Buffer.alloc(32768);
        unknown.write('XXXX', 0, 'latin1');
        boot(unknown);
        if (!waitUntil(() => screenText().includes('PRESS START'), 400)) throw new Error('未知档: 标题未出现');
        tap('START');
        if (!waitUntil(() => screenText().includes('UNSUPPORTED SAVE'), 400)) throw new Error('未知档: 未显示 UNSUPPORTED SAVE');
        if (!waitUntil(() => screenText().includes('UNKNOWN/OLD FORMAT'), 300)) throw new Error('未知档: 格式提示未绘制完成');
        ok(screenText().includes('UNKNOWN/OLD FORMAT'), '未知档提示旧档/未知格式');
        dump('00-unsupported');
        const before = Buffer.from(sram());
        frames(120);
        ok(Buffer.compare(before, Buffer.from(sram())) === 0, '未知档停留 120 帧未写 SRAM');
    }

    console.log('== 流程 A: 全新卡带 (无存档) + 20 字符标签/用户名 + 跑马灯 + 编辑 ==');
    boot();
    waitOk(() => screenText().includes('PRESS START'), '标题画面出现 PRESS START');
    dump('01-title');
    ok(screenText().includes('GB PASSWORD'), '标题文字 GB PASSWORD');
    ok(screenText().includes('V0.3.6.1'), '标题版本 V0.3.6.1');

    if (!tapUntil('START', () => pinScreenReady('NEW PIN', 6), 300, 8)) throw new Error('无法进入设置 PIN 画面');
    ok(true, '进入设置 6 位 PIN 画面');
    dump('02-newpin');
    ok(rowText(0).includes('SECRET VAULT'), 'PIN 画面顶部标题 SECRET VAULT');
    ok(tileAt(0, 0) === 238, '标题下绘制实心分隔线');
    ok(tileAt(1, 2) === 192 && tileAt(2, 2) === 193 && tileAt(3, 2) === 194, 'PIN 显示为 6 个立体分格框');
    ok(tileAt(1, 3) === 195 && tileAt(2, 3) === 211 && tileAt(3, 3) === 197, '当前位显示下划线');
    ok(tileAt(4, 6) === 113 && tileAt(10, 6) === 225 && tileAt(16, 6) === 226, '数字键盘 1/2/3 图形键帽');
    ok(rowText(15).includes('CLR') && rowText(15)[10] === '0' && rowText(15).includes('OK'), '键盘底行 CLR/0/OK');
    ok(padHighlighted(0, 0) && tileAt(2, 5) === 213 && tileAt(2, 6) === 216 && tileAt(2, 7) === 219, '默认高亮按键 1 (整体键帽反白)');

    tap('A');  /* 1 */
    waitOk(() => pinFieldShown('1', 6), '输入 1 后显示在分格框内');
    tap('A');  /* 再按一次 1 */
    waitOk(() => pinFieldShown('11', 6), '再次按 1 后分格框显示 11');
    tapUntil('DOWN', () => padHighlighted(0, 1), 60, 5);
    tapUntil('DOWN', () => padHighlighted(0, 2), 60, 5);
    tapUntil('DOWN', () => padHighlighted(0, 3), 60, 5);
    tap('A');  /* CLR */
    waitOk(() => pinFieldShown('', 6), 'CLR 键清空已输入的 PIN');
    tapUntil('UP', () => padHighlighted(0, 2), 60, 5);
    tapUntil('UP', () => padHighlighted(0, 1), 60, 5);
    tapUntil('UP', () => padHighlighted(0, 0), 60, 5);
    enterPin('123456');
    waitOk(() => pinScreenReady('CONFIRM PIN', 6), '进入确认 PIN 画面');
    enterPin('123456');
    waitOk(() => screenText().includes('VAULT MENU'), '设置完成进入主菜单');
    dump('03-menu');

    let s = sram();
    ok(rom[0x149] === 0x03, 'ROM 头声明 32KB SRAM (0x149 = 0x03)');
    ok(s.length === 32768, 'SRAM 快照 = 32768 字节 (4 x 8KB 存储体)');
    ok(String.fromCharCode(s[0], s[1], s[2], s[3]) === 'PMG4', 'SRAM MAGIC = PMG4');
    ok(s[4] === 8 && s[5] === 9 && s[6] === 0 && s[7] === 1 && s[8] === 2 && s[9] === 3,
        'PIN 123456 以 +7 混淆存储 (8 9 0 1 2 3)');
    ok(s[10] === 0, '错误次数 = 0 (偏移 0x0A)');
    ok((s[12] & 1) === 1, '已设置 PIN 标志 = 1 (偏移 0x0C)');
    ok(checksumOk(s), 'SRAM 全局校验和正确 (16 + 255*80 = 20416 字节)');
    ok(16 + 255 * REC_SZ === 20416, '槽位区总长 = 20416 字节 (跨存储体 0/1/2)');

    /* 账号 1: 普通标签 + 用户名 */
    menuGoto(1);
    if (!tapUntil('A', () => kbScreenReady('LABEL (MAX 20)'), 200, 6)) throw new Error('ADD ACCOUNT 未进入标签输入');
    ok(true, 'ADD ACCOUNT 直接进入标签输入 (自动分配空槽)');
    typeText('LABEL (MAX 20)', 'EMAIL', () => kbScreenReady('USERNAME (MAX 20)'));
    ok(true, '标签 EMAIL 输入完成, 进入用户名输入画面');
    typeText('USERNAME (MAX 20)', 'BOB', () => kbScreenReady('PASSWORD (MAX 36)'));
    ok(true, '用户名 BOB 输入完成, 进入密码输入画面');
    typeText('PASSWORD (MAX 36)', 'ABC', () => screenText().includes('VAULT MENU'));
    ok(true, '密码 ABC 输入完成, 保存并回到主菜单');
    dump('05-menu-after-save');

    s = sram();
    let r0 = recOf(s, 0);
    ok(r0.used === 0xA5, '槽位 1 used = 0xA5');
    ok(r0.llen === 5 && r0.label.startsWith('EMAIL'), '槽位 1 标签 = EMAIL');
    ok(r0.ulen === 3 && r0.user.startsWith('BOB'), '槽位 1 用户名 = BOB');
    ok(r0.plen === 3 && r0.pass.startsWith('ABC'), '槽位 1 密码 = ABC');
    ok(checksumOk(s), '写入槽位后 SRAM 校验和正确');

    /* 账号 2: 20 字符标签 + 20 字符小写用户名 + 36 位混合密码 */
    const LONGLBL = 'ABCDEFGHIJKLMNOPQRST';       /* 20 */
    const LONGUSR = 'abcdefghijklmnopqrst';      /* 20 */
    menuGoto(1);
    if (!tapUntil('A', () => kbScreenReady('LABEL (MAX 20)'), 200, 6)) throw new Error('未进入标签输入 (第二账号)');
    typeText('LABEL (MAX 20)', LONGLBL, () => kbScreenReady('USERNAME (MAX 20)'));
    ok(true, '20 字符标签输入完成');
    typeText('USERNAME (MAX 20)', LONGUSR, () => kbScreenReady('PASSWORD (MAX 36)'));
    ok(true, '20 字符小写用户名输入完成 (键盘 v2)');
    const LONG = 'aB3!xY9#qW2$eR5%tY8&uI1(uO4)Kz6_@b?!';
    typeText('PASSWORD (MAX 36)', LONG, () => screenText().includes('VAULT MENU'));
    ok(true, '36 位混合大小写 + 符号密码输入完成并保存');
    dump('05b-menu-after-mixed');

    s = sram();
    const r1 = recOf(s, 1);
    ok(r1.used === 0xA5 && r1.llen === 20 && r1.label === LONGLBL, '槽位 2 标签 = 20 字符完整保存');
    ok(r1.ulen === 20 && r1.user === LONGUSR, '槽位 2 用户名 = 20 字符完整保存');
    ok(r1.plen === 36 && r1.pass === LONG, '槽位 2 密码 = 36 位混合大小写 + 符号');
    ok(checksumOk(s), '写入长字段后 SRAM 校验和正确');

    /* 账号 3: 空用户名 + 随机密码生成器 */
    menuGoto(1);
    if (!tapUntil('A', () => kbScreenReady('LABEL (MAX 20)'), 200, 6)) throw new Error('未进入标签输入 (第三账号)');
    typeText('LABEL (MAX 20)', 'GENACC', () => kbScreenReady('USERNAME (MAX 20)'));
    ok(true, '标签 GENACC 输入完成 (第三账号)');
    if (!tapUntil('START', () => kbScreenReady('PASSWORD (MAX 36)'), 200, 6)) throw new Error('用户名留空后未进入密码页');
    ok(true, '用户名可留空 (START 直接跳过)');
    ok(rowText(2).slice(1, 16) === '_'.padEnd(15, ' '), '密码页初始为空 (仅光标)');

    const genFieldOk = () => rowText(2).slice(2, 16).trim() !== '';
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
    frames(5);
    const shownTail = rowText(2).slice(1, 15);

    frames(5);
    tap('START');
    waitOk(() => screenText().includes('VAULT MENU'), '生成密码保存后回到主菜单', 1200);

    s = sram();
    const r2 = recOf(s, 2);
    ok(r2.used === 0xA5 && r2.llen === 6 && r2.label.startsWith('GENACC'), '槽位 3 标签 = GENACC');
    ok(r2.ulen === 0, '槽位 3 用户名为空');
    ok(r2.plen === 16, '生成密码长度 = 16');
    const pass2 = r2.pass.slice(0, 16);
    ok([...pass2].every(c => GEN_ALL.includes(c)), '生成密码字符全部来自安全字符集 (无 0O1lI)');
    ok(/[A-Z]/.test(pass2) && /[a-z]/.test(pass2) && /[2-9]/.test(pass2) && /[.\-_@!#$%&()?+]/.test(pass2),
        '生成密码含大写/小写/数字/符号四类字符');
    ok(pass2.slice(2, 16) === shownTail, '保存的密码与画面显示尾部一致');
    ok(checksumOk(s), '生成密码写入后 SRAM 校验和正确');

    /* 列表: 位置 + 14 字符窗口 + 选中行跑马灯 */
    menuGoto(0);
    if (!tapUntil('A', () => listReady(), 200, 6)) throw new Error('未进入账号列表');
    ok(true, '进入账号列表');
    dump('06-list');
    ok(rowText(0).includes('001/255'), '列表右上角显示 001/255');
    ok(rowText(3).includes('001 EMAIL'), '列表第 1 行显示 001 EMAIL');
    ok(rowText(5).includes('003 GENACC'), '列表第 3 行显示 003 GENACC');
    ok(rowText(4).includes('002 '), '列表第 2 行显示 002');

    /* 选中 20 字符标签: 先滚到尾部再滚回头部 */
    if (!tapUntil('DOWN', () => listSel() === 1, 120, 4)) throw new Error('无法选中第 2 条');
    ok(true, '选中 20 字符标签的条目 (跑马灯)');
    ok(waitUntil(() => rowWindow(4) === 'GHIJKLMNOPQRST', 900), '跑马灯滚动显示标签尾部');
    ok(waitUntil(() => rowWindow(4) === 'ABCDEFGHIJKLMN', 900), '跑马灯回滚显示标签头部');
    dump('06b-list-marquee');

    listGoto(0);
    if (!tapUntil('A', () => screenText().includes(detailTitle(0)), 200, 6)) throw new Error('未进入详情页');
    frames(5);
    ok(true, '进入详情页 (SLOT 001/255)');
    dump('07-detail-masked');
    ok(rowText(3).includes('EMAIL'), '详情页标签行显示 EMAIL');
    ok(rowText(5).includes('BOB'), '详情页用户名行显示 BOB');
    ok(rowText(7).includes('***'), '密码默认打码 (第 7 行)');

    hold('A', 40);
    dump('08-detail-reveal', ['A']);
    ok(rowText(7).includes('ABC'), '按住 A 显示明文 ABC');
    frames(10);

    /* SELECT+START 编辑: 标签保留, 用户名追加 X, 密码保留 */
    chord(['SELECT', 'START']);
    waitOk(() => kbScreenWith('LABEL (MAX 20)', 'EMAIL'), 'SELECT+START 进入编辑 (标签已预填)');
    ok(tapUntil('START', () => kbScreenWith('USERNAME (MAX 20)', 'BOB'), 200, 6), '标签确认后进入用户名编辑 (已预填)');
    typeMore('USERNAME (MAX 20)', 'BOB', 'X', () => kbScreenWith('PASSWORD (MAX 36)', 'ABC'));
    ok(true, '用户名追加为 BOBX, 密码页已预填 ABC');
    if (!tapUntil('START', () => screenText().includes(detailTitle(0)) && rowText(5).includes('BOBX'), 400, 6)) {
        throw new Error('编辑保存后未回到详情页');
    }
    ok(true, '编辑保存后详情页显示 BOBX');
    dump('08b-detail-edited');
    s = sram();
    r0 = recOf(s, 0);
    ok(r0.llen === 5 && r0.label.startsWith('EMAIL'), '编辑后标签保持 EMAIL');
    ok(r0.ulen === 4 && r0.user.startsWith('BOBX'), '编辑后用户名 = BOBX');
    ok(r0.plen === 3 && r0.pass.startsWith('ABC'), '编辑后密码保持 ABC');
    ok(checksumOk(s), '编辑后 SRAM 校验和正确');

    /* 单独 START 仍是删除确认 */
    if (!tapUntil('START', () => screenText().includes('DELETE THIS SLOT?'), 200, 6)) throw new Error('START 未弹出删除确认');
    ok(true, '单独按 START 弹出删除确认 (组合键不影响)');
    if (!tapUntil('B', () => screenText().includes(detailTitle(0)) && !screenText().includes('DELETE THIS'), 200, 6)) throw new Error('B 未取消删除');
    ok(recOf(sram(), 0).used === 0xA5, '取消删除后记录仍在');

    if (!tapUntil('B', () => listReady(), 200, 6)) throw new Error('未能返回列表');
    ok(true, '详情页返回列表');
    if (!tapUntil('B', () => screenText().includes('VAULT MENU'), 200, 6)) throw new Error('未能返回菜单');
    ok(true, '列表返回主菜单');

    menuGoto(3);
    if (!tapUntil('A', () => pinScreenReady('ENTER PIN', 6), 200, 6)) throw new Error('未能进入解锁画面');
    ok(true, '锁库后进入解锁画面');
    dump('09-locked-to-unlock');

    const save1 = sram();
    fs.writeFileSync(path.join(OUT, 'save1.bin'), Buffer.from(save1));
    console.log('已保存 SRAM 快照 save1.bin (' + save1.length + ' 字节)');

    console.log();
    console.log('== 流程 B: 错误 PIN 三次 -> SYSTEM LOCKED -> 擦除恢复 ==');
    boot(save1);
    if (!tapUntil('START', () => pinScreenReady('ENTER PIN', 6), 300, 8)) throw new Error('未进入解锁画面');
    ok(true, '重载存档后进入解锁画面');
    dump('10-unlock');

    for (let i = 0; i < 3; i++) {
        enterPin('000000');
        if (i < 2) waitOk(() => pinScreenReady('ENTER PIN', 6), '第 ' + (i + 1) + ' 次错误后回到解锁画面');
    }
    waitOk(() => screenText().includes('SYSTEM LOCKED'), '错误 3 次进入 SYSTEM LOCKED 彩蛋', 900);
    dump('11-system-locked');
    s = sram();
    ok(s[10] === 3, '错误次数持久化 = 3 (偏移 0x0A)');

    /* 按住 A+B+SELECT+START 三秒恢复 (joypad 轮询 + 硬件定时器计时) */
    const COMBO = ['A', 'B', 'SELECT', 'START'];
    hold4(COMBO, 100);
    ok(!screenText().includes('NEW PIN'), '长按不足 3 秒不触发擦除');
    let recovered = false;
    for (let f = 0; f < 1200 && !recovered; f++) {
        hold4(COMBO, 1);
        recovered = screenText().includes('NEW PIN');
    }
    ok(recovered, '长按 3 秒后擦除, 恢复首次设置流程');
    frames(5);
    waitOk(() => pinScreenReady('NEW PIN', 6), '擦除后稳定在设置 PIN 画面', 300);
    dump('12-after-recovery');
    s = sram();
    ok((s[12] & 1) === 0, '擦除后 PIN 标志 = 0');
    ok(s[10] === 0, '擦除后错误次数 = 0');
    ok(recOf(s, 0).used === 0, '擦除后槽位 1 为空');
    ok(recOf(s, 1).used === 0, '擦除后槽位 2 为空');
    ok(recOf(s, 2).used === 0, '擦除后槽位 3 为空');
    ok(checksumOk(s), '擦除后 SRAM 校验和正确');
    ok(String.fromCharCode(s[0], s[1], s[2], s[3]) === 'PMG4', '擦除后 MAGIC 仍在');

    console.log();
    console.log('== 流程 C: 用 save1 重新上电 -> 正确 PIN -> 数据仍在 ==');
    boot(save1);
    if (!tapUntil('START', () => pinScreenReady('ENTER PIN', 6), 300, 8)) throw new Error('未进入解锁画面');
    ok(true, '重新上电进入解锁画面');
    enterPin('123456');
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
    frames(5);
    hold('A', 40);
    dump('15-reveal-persisted', ['A']);
    ok(rowText(7).includes('ABC') && rowText(5).includes('BOBX'), '断电重开后密码 ABC / 用户名 BOBX 仍在');
    frames(10);

    if (!tapUntil('B', () => listReady(), 200, 6)) throw new Error('未返回列表');
    listGoto(1);
    if (!tapUntil('A', () => screenText().includes(detailTitle(1)), 200, 6)) throw new Error('未进入长密码详情');
    frames(5);
    const stars7 = (rowText(7).match(/\*/g) || []).length;
    const stars8 = (rowText(8).match(/\*/g) || []).length;
    ok(stars7 === 18 && stars8 === 18, '36 位密码打码 (行7=18颗 行8=18颗)');
    hold('A', 40);
    dump('15b-detail-long-reveal', ['A']);
    ok(rowText(7).trim() + rowText(8).trim() === LONG, '按住 A 显示 36 位明文 (两行)');
    frames(10);

    console.log();
    console.log('== 流程 D: 255 槽滚动列表 + 末槽写入/删除 (合成 20 账号存档) ==');
    boot(buildSyntheticSave(20));
    if (!tapUntil('START', () => pinScreenReady('ENTER PIN', 6), 300, 8)) throw new Error('流程 D: 未进入解锁画面');
    enterPin('123456');
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

    /* 右翻页 12: 2 -> 14 (超出窗口, 滚动) */
    if (!tapUntil('RIGHT', () => rowText(0).includes('014/255') && listSel() === 11 && rowText(3).includes('003 SLOT03'), 150, 3)) throw new Error('流程 D: 右翻页失败');
    ok(listSel() === 11, '右翻页后高亮位于窗口末行');
    ok(rowText(3).includes('003 SLOT03'), '窗口滚动后首行 = 003 SLOT03');
    ok(rowText(14).includes('014 SLOT14'), '高亮行 = 014 SLOT14');

    /* 再右翻页 12: 14 -> 26 */
    if (!tapUntil('RIGHT', () => rowText(0).includes('026/255') && listSel() === 11 && rowText(3).includes('015 SLOT15'), 150, 3)) throw new Error('流程 D: 右翻页 2 失败');
    ok(listSel() === 11, '再右翻页后高亮位于窗口末行');
    ok(rowText(3).includes('015 SLOT15'), '窗口滚动后首行 = 015 SLOT15');
    ok(rowText(14).includes('026 EMPTY'), '高亮行 = 026 EMPTY (合成档仅前 20 条)');

    /* 左翻页 12: 26 -> 14 */
    if (!tapUntil('LEFT', () => rowText(0).includes('014/255') && listSel() === 0 && rowText(3).includes('014 SLOT14'), 150, 3)) throw new Error('流程 D: 左翻页失败');
    ok(listSel() === 0, '左翻页后高亮在窗口首行');
    ok(rowText(3).includes('014 SLOT14'), '高亮行 = 014 SLOT14');

    /* 上移 2 格后打开第 12 号槽详情 */
    if (!tapUntil('UP', () => listSel() === 0 && rowText(0).includes('013/255'), 150, 3)) throw new Error('流程 D: 上移失败');
    if (!tapUntil('UP', () => listSel() === 0 && rowText(0).includes('012/255'), 150, 3)) throw new Error('流程 D: 上移 2 失败');
    if (!tapUntil('A', () => screenText().includes(detailTitle(11)), 200, 6)) throw new Error('流程 D: 未打开 SLOT12 详情');
    frames(5);
    ok(true, '打开第 12 号槽位详情 (SLOT 012/255)');
    ok(rowText(5).includes('USER12'), '详情页用户名 = USER12');
    hold('A', 40);
    ok(rowText(7).includes('P12'), '按住 A 显示明文 P12');
    frames(10);
    if (!tapUntil('B', () => listReady(), 200, 6)) throw new Error('流程 D: 详情返回失败');
    ok(listSel() === 0, '返回列表后高亮位置保持');

    /* 连续右翻页直达底部 255: 每次目标精确校验 (丢帧的点按自动重发, 不会重跳) */
    let selJ = 11, viewJ = 11;
    let bottomOk = true;
    while (selJ < 254) {
        selJ = Math.min(selJ + 12, 254);
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
    waitUntil(() => rowText(0).includes('255/255') && rowText(14).includes('255 EMPTY'), 60);
    ok(rowText(14).includes('255 EMPTY'), '末行 = 255 EMPTY');
    dump('17-list-window-bottom');
    tap('RIGHT');
    waitUntil(() => rowText(0).includes('255/255') && rowText(14).includes('255 EMPTY'), 60);
    ok(rowText(0).includes('255/255'), '底部再右跳保持 255/255');

    if (!tapUntil('A', () => kbScreenReady('LABEL (MAX 20)'), 200, 6)) throw new Error('流程 D: 空槽 A 未进入标签输入');
    ok(true, '在 255 号空槽按 A 直接进入添加流程');
    dump('18-add-at-255');
    if (!tapUntil('SELECT', () => listReady(), 200, 6)) throw new Error('流程 D: 标签页 SELECT 未取消');
    ok(rowText(0).includes('255/255'), '取消添加后回到列表且位置保持');

    if (!tapUntil('A', () => kbScreenReady('LABEL (MAX 20)'), 200, 6)) throw new Error('流程 D: 二次进入标签输入失败');
    typeText('LABEL (MAX 20)', 'LAST', () => kbScreenReady('USERNAME (MAX 20)'));
    if (!tapUntil('START', () => kbScreenReady('PASSWORD (MAX 36)'), 200, 6)) throw new Error('流程 D: 空用户名未进入密码页');
    if (!tapUntil('SELECT', () => rowText(2).slice(2, 16).trim() !== '', 200, 6)) throw new Error('流程 D: 255 号槽密码生成失败');
    frames(5);
    const tail254 = rowText(2).slice(1, 15);
    frames(5);
    tap('START');
    waitOk(() => listReady(), '保存后回到列表 (255 号槽)', 1200);
    ok(rowText(0).includes('255/255'), '位置仍是 255/255');
    ok(rowText(14).includes('255 LAST'), '行文本更新为 255 LAST');

    s = sram();
    const r254 = recOf(s, 254);
    ok(r254.used === 0xA5 && r254.llen === 4 && r254.label.startsWith('LAST'), '第 255 槽标签 = LAST');
    ok(r254.ulen === 0, '第 255 槽用户名为空');
    ok(r254.plen === 16 && r254.pass.slice(2, 16) === tail254, '第 255 槽密码 = 生成的 16 位 (与显示一致)');
    ok(16 + 254 * REC_SZ === 20336, '第 255 槽起始地址 = 20336 (存储体 2)');
    ok(Math.floor((16 + 254 * REC_SZ) / 8192) === 2, '第 255 槽位于第 3 个 8KB 存储体');
    ok(checksumOk(s), '跨 3 个存储体 (20416 字节) 校验和正确');

    /* 详情删除 255 号槽 */
    if (!tapUntil('A', () => screenText().includes(detailTitle(254)), 200, 6)) throw new Error('流程 D: 未打开 SLOT 255 详情');
    frames(5);
    if (!tapUntil('START', () => screenText().includes('DELETE THIS SLOT?'), 200, 6)) throw new Error('流程 D: 未出现删除确认');
    ok(true, '详情页 START 弹出删除确认');
    if (!tapUntil('A', () => listReady(), 400, 6)) throw new Error('流程 D: 删除后未返回列表');
    ok(rowText(14).includes('255 EMPTY'), '删除后 255 号槽恢复 EMPTY');
    s = sram();
    ok(recOf(s, 254).used === 0, '删除后 255 号槽 used = 0');
    ok(checksumOk(s), '删除后 SRAM 校验和正确');

    console.log();
    console.log('== 流程 E: v0.3 (PMG3) 存档迁移 -> 新 PIN -> 数据保留 ==');
    const v03 = fs.readFileSync(V03_SAVE);
    ok(String.fromCharCode(v03[0], v03[1], v03[2], v03[3]) === 'PMG3', '迁移样本为 PMG3 旧档');
    ok(legacyChecksumOk(v03), '迁移样本旧校验和正确');
    const old0 = legacyRecOf(v03, 0), old1 = legacyRecOf(v03, 1), old2 = legacyRecOf(v03, 2);
    ok(old0.label.startsWith('EMAIL') && old0.pass.startsWith('ABC'), '旧档槽位 1 = EMAIL / ABC');
    ok(old1.llen === 5 && old1.plen === 36, '旧档槽位 2 = MIXED / 36 位密码');
    ok(old2.label.startsWith('GENACC') && old2.plen === 16, '旧档槽位 3 = GENACC / 16 位密码');

    boot(v03);
    if (!tapUntil('START', () => pinScreenReady('OLD 4-DIGIT PIN', 4), 500, 8)) throw new Error('迁移: 未进入旧 PIN 画面');
    ok(true, '检测到 v0.3 存档, 要求输入旧 4 位 PIN');
    dump('19-migrate-oldpin');

    enterPin('0000');
    ok(waitUntil(() => sram()[8] === 1, 300), '旧 PIN 输错后错误次数持久化 = 1');
    ok(waitUntil(() => pinScreenReady('OLD 4-DIGIT PIN', 4), 300), '错误后重新显示旧 PIN 画面');
    ok(legacyChecksumOk(sram()), '旧档错误次数写入后旧校验和仍正确');
    enterPin('1234');
    waitOk(() => pinScreenReady('NEW PIN', 6), '旧 PIN 正确后要求设置新 6 位 PIN', 600);
    enterPin('654321');
    waitOk(() => pinScreenReady('CONFIRM PIN', 6), '迁移: 进入新 PIN 确认画面');
    enterPin('654321');
    waitOk(() => screenText().includes('VAULT MENU'), '迁移完成并直接进入主菜单', 3000);
    dump('20-migrated-menu');

    s = sram();
    ok(String.fromCharCode(s[0], s[1], s[2], s[3]) === 'PMG4', '迁移后 MAGIC = PMG4');
    ok(s[4] === 3 && s[5] === 2 && s[6] === 1 && s[7] === 0 && s[8] === 9 && s[9] === 8,
        '新 PIN 654321 混淆存储 (3 2 1 0 9 8)');
    ok(s[10] === 0, '迁移后错误次数 = 0');
    ok((s[12] & 1) === 1, '迁移后 PIN 标志 = 1');
    ok(checksumOk(s), '迁移后新布局校验和正确');
    const m0 = recOf(s, 0), m1 = recOf(s, 1), m2 = recOf(s, 2);
    ok(m0.used === 0xA5 && m0.llen === 5 && m0.label.startsWith('EMAIL') && m0.plen === 3 && m0.pass.startsWith('ABC'),
        '迁移后槽位 1 标签/密码保留');
    ok(m0.ulen === 0, '迁移后槽位 1 用户名留空');
    ok(m1.llen === 5 && m1.label.startsWith('MIXED') && m1.plen === 36 && m1.pass === old1.pass,
        '迁移后槽位 2 的 36 位密码逐字节保留');
    ok(m2.label.startsWith('GENACC') && m2.plen === 16 && m2.pass === old2.pass,
        '迁移后槽位 3 的生成密码逐字节保留');
    let tempZero = true;
    for (let i = MIG_TMP; i < 32768; i++) if (s[i] !== 0) { tempZero = false; break; }
    ok(tempZero, '迁移暂存区已清零 (不留密码副本)');
    const save2 = sram();
    fs.writeFileSync(path.join(OUT, 'save2-migrated.bin'), Buffer.from(save2));

    /* 迁移后的旧记录补填用户名 (SELECT+START 编辑) */
    menuGoto(0);
    if (!tapUntil('A', () => listReady(), 200, 6)) throw new Error('迁移: 未进入列表');
    dump('21-migrated-list');
    ok(screenText().includes('001 EMAIL') && screenText().includes('003 GENACC'), '迁移后列表显示旧账号');
    if (!tapUntil('A', () => screenText().includes(detailTitle(0)), 200, 6)) throw new Error('迁移: 未打开 EMAIL 详情');
    frames(5);
    ok(rowText(4).includes('USER:') && rowText(5).trim() === '', '迁移记录用户名行为空');
    chord(['SELECT', 'START']);
    waitOk(() => kbScreenWith('LABEL (MAX 20)', 'EMAIL'), '迁移记录可进入编辑');
    if (!tapUntil('START', () => kbScreenReady('USERNAME (MAX 20)'), 200, 6)) throw new Error('迁移: 用户名页未出现');
    typeText('USERNAME (MAX 20)', 'OWNER', () => kbScreenWith('PASSWORD (MAX 36)', 'ABC'));
    if (!tapUntil('START', () => rowText(5).includes('OWNER'), 400, 6)) throw new Error('迁移: 补填用户名后未回详情');
    ok(true, '迁移记录补填用户名 OWNER');
    s = sram();
    ok(recOf(s, 0).ulen === 5 && recOf(s, 0).user.startsWith('OWNER'), '补填用户名写入 SRAM');
    ok(checksumOk(s), '补填用户名后校验和正确');
    fs.writeFileSync(path.join(OUT, 'save3-migrated-user.bin'), Buffer.from(s));

    /* 用迁移后的存档重新上电, 新 PIN 解锁 */
    boot(s);
    if (!tapUntil('START', () => pinScreenReady('ENTER PIN', 6), 300, 8)) throw new Error('迁移: 重开未进入解锁');
    enterPin('654321');
    waitOk(() => screenText().includes('VAULT MENU'), '迁移后新 PIN 可正常解锁');
    menuGoto(0);
    if (!tapUntil('A', () => listReady(), 200, 6)) throw new Error('迁移: 重开未进入列表');
    listGoto(0);
    if (!tapUntil('A', () => screenText().includes(detailTitle(0)), 200, 6)) throw new Error('迁移: 重开未进入详情');
    frames(5);
    ok(rowText(5).includes('OWNER'), '重开后迁移记录的用户名 OWNER 仍在');
    hold('A', 40);
    ok(rowText(7).includes('ABC'), '重开后迁移记录的密码 ABC 仍在');

    console.log();
    console.log('== 流程 F: 四项优化 (列表视图/连滚/生成器设置/自动锁屏/回收站) ==');
    boot(save1);
    if (!tapUntil('START', () => pinScreenReady('ENTER PIN', 6), 300, 8)) throw new Error('流程 F: 未进入解锁画面');
    enterPin('123456');
    waitOk(() => screenText().includes('VAULT MENU'), '流程 F: 解锁进入主菜单');

    /* --- 设置: 12 位 / 无符号 / 1 分钟自动锁 --- */
    menuGoto(5);
    if (!tapUntil('A', () => screenText().includes('SETTINGS') && settingsSel() === 0, 200, 6)) throw new Error('流程 F: 未进入设置画面');
    frames(3);
    ok(rowText(4).includes('16') && rowText(6).includes('ON'), '设置默认值 = 16 位 / 符号 ON');
    if (!tapUntil('A', () => rowText(4).includes('24'), 80, 6)) throw new Error('流程 F: 长度未切到 24');
    if (!tapUntil('A', () => rowText(4).includes('32'), 80, 6)) throw new Error('流程 F: 长度未切到 32');
    if (!tapUntil('A', () => rowText(4).includes('12'), 80, 6)) throw new Error('流程 F: 长度未切到 12');
    if (!tapUntil('DOWN', () => settingsSel() === 1, 80, 6)) throw new Error('流程 F: 未移到符号行');
    if (!tapUntil('A', () => rowText(6).includes('OFF'), 80, 6)) throw new Error('流程 F: 符号未关闭');
    if (!tapUntil('DOWN', () => settingsSel() === 2, 80, 6)) throw new Error('流程 F: 未移到锁屏行');
    if (!tapUntil('A', () => rowText(8).includes('5 MIN'), 80, 6)) throw new Error('流程 F: 锁屏未切到 5 分钟');
    if (!tapUntil('A', () => rowText(8).includes('10 MIN'), 80, 6)) throw new Error('流程 F: 锁屏未切到 10 分钟');
    if (!tapUntil('A', () => rowText(8).includes('1 MIN'), 80, 6)) throw new Error('流程 F: 锁屏未切到 1 分钟');
    dump('22-settings');
    if (!tapUntil('B', () => screenText().includes('VAULT MENU'), 80, 6)) throw new Error('流程 F: 设置返回失败');

    s = sram();
    ok((s[13] & 0x80) !== 0 && (s[13] & 3) === 0 && (s[13] & 4) === 0, '设置写入头部: 12 位 + 符号 OFF + 有效位');
    ok(s[14] === 0, '自动锁屏 = 1 分钟 (头部 0x0E = 0)');
    ok(s[15] === 0, '列表默认标签视图 (头部 0x0F = 0)');
    ok(checksumOk(s), '设置写入后 SRAM 校验和正确');

    /* --- 12 位无符号随机密码 --- */
    menuGoto(1);
    if (!tapUntil('A', () => kbScreenReady('LABEL (MAX 20)'), 200, 6)) throw new Error('流程 F: 未进入标签输入');
    typeText('LABEL (MAX 20)', 'SHORT', () => kbScreenReady('USERNAME (MAX 20)'));
    if (!tapUntil('START', () => kbScreenReady('PASSWORD (MAX 36)'), 200, 6)) throw new Error('流程 F: 用户名跳过失败');
    if (!tapUntil('SELECT', () => rowText(2).slice(2, 16).trim() !== '', 200, 6)) throw new Error('流程 F: 生成密码失败');
    const shortShown = rowText(2).slice(1, 13);
    ok([...shortShown].every(c => GEN_ALL_NS.includes(c)), '画面上的 12 位密码无符号');
    frames(5);
    tap('START');
    waitOk(() => screenText().includes('VAULT MENU'), '流程 F: 12 位密码保存后回菜单', 1200);

    s = sram();
    const rShort = recOf(s, 3);
    ok(rShort.used === 0xA5 && rShort.llen === 5 && rShort.label.startsWith('SHORT'), '槽位 4 = SHORT');
    ok(rShort.ulen === 0, 'SHORT 用户名为空');
    ok(rShort.plen === 12, '生成密码长度 = 12 (设置生效)');
    ok([...rShort.pass.slice(0, 12)].every(c => GEN_ALL_NS.includes(c)), '保存密码只含大小写/数字 (符号关闭生效)');
    ok(/[A-Z]/.test(rShort.pass) && /[a-z]/.test(rShort.pass) && /[2-9]/.test(rShort.pass),
        '12 位密码含大写/小写/数字');
    ok(rShort.pass.slice(0, 12) === shortShown, '保存密码与画面一致');
    ok(checksumOk(s), '生成器设置写入后校验和正确');

    /* --- 列表: SELECT 切换标签/用户名 --- */
    menuGoto(0);
    if (!tapUntil('A', () => listReady(), 200, 6)) throw new Error('流程 F: 未进入列表');
    ok(rowText(3).includes('001 EMAIL'), '列表默认显示标签 (001 EMAIL)');
    ok(rowText(16).includes('SEL=USER'), '标签视图提示 SEL=USER');
    if (!tapUntil('SELECT', () => rowText(16).includes('SEL=LABEL') && rowText(3).includes('001 BOBX'), 150, 6))
        throw new Error('流程 F: SELECT 未切到用户名视图');
    ok(rowText(3).includes('001 BOBX'), '用户名视图显示 001 BOBX');
    ok(rowText(5).includes('003 -'), '空用户名显示 003 -');
    ok(rowText(6).includes('004 -'), 'SHORT 空用户名显示 004 -');
    dump('23-list-user-view');
    s = sram();
    ok(s[15] === 1, '列表视图模式持久化 (头部 0x0F = 1)');
    ok(checksumOk(s), '切换列表视图后校验和正确');

    /* --- 按住 DOWN 连续滚动 --- */
    let mPos = rowText(0).match(/(\d{3})\/255/);
    const startPos = mPos ? parseInt(mPos[1], 10) : 0;
    hold('DOWN', 220);
    frames(10);
    mPos = rowText(0).match(/(\d{3})\/255/);
    const endPos = mPos ? parseInt(mPos[1], 10) : 0;
    ok(endPos - startPos >= 4, '按住 DOWN 连续滚动 (从 ' + startPos + ' 到 ' + endPos + ')');
    frames(40);
    ok(rowText(0).includes(String(endPos).padStart(3, '0') + '/255'), '松开后停止滚动');
    dump('24-list-hold-scroll');

    if (!tapUntil('SELECT', () => rowText(16).includes('SEL=USER'), 150, 6)) throw new Error('流程 F: 未切回标签视图');
    if (!tapUntil('B', () => screenText().includes('VAULT MENU'), 200, 6)) throw new Error('流程 F: 列表返回菜单');
    ok(sram()[15] === 0, '切回标签视图持久化 (0x0F = 0)');

    /* --- 自动锁屏: 1 分钟无操作 --- */
    frames(3000);
    ok(screenText().includes('VAULT MENU'), '自动锁屏: 3000 帧无操作仍在菜单 (<1 分钟)');
    tap('DOWN');
    frames(3000);
    ok(screenText().includes('VAULT MENU'), '自动锁屏: 按键重置计时, 再 3000 帧仍在菜单');
    if (!waitUntil(() => pinScreenReady('ENTER PIN', 6), 5000)) throw new Error('流程 F: 1 分钟闲置未自动锁屏');
    ok(true, '闲置 1 分钟自动回到锁库画面');
    dump('25-auto-locked');
    enterPin('123456');
    waitOk(() => screenText().includes('VAULT MENU'), '流程 F: 自动锁屏后可重新解锁');

    /* --- 快速锁定: 菜单长按 B 1.5 秒; 短按 B 仍为返回 --- */
    frames(3);
    let quickLocked = false;
    for (let t = 0; t < 240 && !quickLocked; t++) {
        gb.pressKey('B');
        gb.doFrame();
        quickLocked = screenText().includes('ENTER PIN');
    }
    if (!quickLocked) quickLocked = waitUntil(() => screenText().includes('ENTER PIN'), 240);
    ok(quickLocked, '长按 B 1.5 秒快速锁定回 PIN 画面');
    if (!quickLocked) throw new Error('快速锁定未触发');
    enterPin('123456');
    waitOk(() => screenText().includes('VAULT MENU'), '流程 F: 快速锁定后可重新解锁');

    /* --- 修改 PIN: 先验证旧 PIN; 输入新 PIN 中闲置同样自动锁屏 --- */
    menuGoto(2);
    if (!tapUntil('A', () => pinScreenReady('OLD PIN', 6), 300, 6)) throw new Error('流程 F: 未进入旧 PIN 验证画面');
    enterPin('123456');
    if (!waitUntil(() => pinScreenReady('NEW PIN', 6), 500)) throw new Error('流程 F: 旧 PIN 正确后未进入新 PIN');
    ok(true, '修改 PIN 先验证旧 PIN 成功');
    if (!waitUntil(() => pinScreenReady('ENTER PIN', 6), 9000)) throw new Error('流程 F: 改 PIN 中闲置未自动锁屏');
    ok(true, '修改新 PIN 画面闲置 1 分钟自动锁屏');
    s = sram();
    ok(s[4] === 8 && s[5] === 9 && s[9] === 3, '改 PIN 闲置锁屏后旧 PIN 未被修改 (混淆值不变)');
    enterPin('123456');
    waitOk(() => screenText().includes('VAULT MENU'), '流程 F: 改 PIN 闲置锁屏后可重新解锁');

    /* --- 错误旧 PIN 不修改, 正确旧 PIN 才能换新 PIN --- */
    menuGoto(2);
    if (!tapUntil('A', () => pinScreenReady('OLD PIN', 6), 300, 6)) throw new Error('流程 F: 未进入旧 PIN (错误路径)');
    enterPin('654321');
    waitOk(() => screenText().includes('VAULT MENU'), '错误旧 PIN 被拒绝并返回菜单', 1200);
    s = sram();
    ok(s[4] === 8 && s[5] === 9 && s[9] === 3, '错误旧 PIN 未修改 PIN');

    menuGoto(2);
    if (!tapUntil('A', () => pinScreenReady('OLD PIN', 6), 300, 6)) throw new Error('流程 F: 未进入旧 PIN (成功路径)');
    enterPin('123456');
    if (!waitUntil(() => pinScreenReady('NEW PIN', 6), 500)) throw new Error('流程 F: 成功路径未进入新 PIN');
    enterPin('111111');
    if (!waitUntil(() => pinScreenReady('CONFIRM PIN', 6), 500)) throw new Error('流程 F: 未进入确认 PIN');
    enterPin('111111');
    waitOk(() => screenText().includes('VAULT MENU'), '正确旧 PIN 修改为新 PIN', 1500);
    s = sram();
    ok(s[4] === 8 && s[5] === 8 && s[9] === 8, '新 PIN 混淆值已保存');

    /* 改回 123456, 后面的断电/迁移用例继续使用原 PIN */
    menuGoto(2);
    if (!tapUntil('A', () => pinScreenReady('OLD PIN', 6), 300, 6)) throw new Error('流程 F: 未进入旧 PIN (改回)');
    enterPin('111111');
    if (!waitUntil(() => pinScreenReady('NEW PIN', 6), 500)) throw new Error('流程 F: 改回时未进入新 PIN');
    enterPin('123456');
    if (!waitUntil(() => pinScreenReady('CONFIRM PIN', 6), 500)) throw new Error('流程 F: 改回时未进入确认 PIN');
    enterPin('123456');
    waitOk(() => screenText().includes('VAULT MENU'), 'PIN 改回 123456', 1500);
    s = sram();
    ok(s[4] === 8 && s[5] === 9 && s[9] === 3, 'PIN 已恢复为 123456');

    /* --- 回收站: 删除 -> 断电 -> 恢复 --- */
    menuGoto(0);
    if (!tapUntil('A', () => listReady(), 200, 6)) throw new Error('流程 F: 未进入列表');
    listGoto(3);
    if (!tapUntil('A', () => screenText().includes(detailTitle(3)), 200, 6)) throw new Error('流程 F: 未打开 SHORT 详情');
    frames(5);
    if (!tapUntil('START', () => screenText().includes('DELETE THIS SLOT?'), 200, 6)) throw new Error('流程 F: 未出现删除确认');
    if (!tapUntil('A', () => listReady(), 600, 6)) throw new Error('流程 F: 删除后未返回列表');
    ok(rowText(6).includes('004 EMPTY'), '删除后列表 004 EMPTY');
    s = sram();
    ok(recOf(s, 3).used === 0, '删除后槽位 4 used = 0');
    ok(String.fromCharCode(s[20416], s[20417], s[20418], s[20419]) === 'BIN1', '回收站头 MAGIC = BIN1');
    ok(s[20420] === 1, '回收站 head = 1');
    ok(s[20432] === 0xA5, '回收站条目 used = 0xA5');
    ok(s[20433] === 5 && String.fromCharCode(...s.slice(20434, 20439)) === 'SHORT', '回收站条目标签 = SHORT');
    ok(binChecksumOk(s), '回收站校验和正确');
    ok(checksumOk(s), '删除后主库校验和仍正确');
    fs.writeFileSync(path.join(OUT, 'save4-bin.bin'), Buffer.from(s));

    boot(s);
    if (!tapUntil('START', () => pinScreenReady('ENTER PIN', 6), 300, 8)) throw new Error('流程 F: 回收站重开未进入解锁');
    enterPin('123456');
    waitOk(() => screenText().includes('VAULT MENU'), '流程 F: 回收站重开解锁');
    menuGoto(4);
    if (!tapUntil('A', () => rowText(0).includes('RECYCLE BIN'), 200, 6)) throw new Error('流程 F: 未进入回收站');
    dump('26-bin');
    ok(rowText(1).includes('01 SHORT'), '回收站显示 01 SHORT (断电后仍在)');
    if (!tapUntil('A', () => screenText().includes('BIN EMPTY'), 600, 6)) throw new Error('流程 F: 恢复失败');
    ok(true, '回收站恢复条目并清空');
    s = sram();
    ok(recOf(s, 3).used === 0xA5 && recOf(s, 3).label.startsWith('SHORT'), '恢复后槽位 4 = SHORT');
    ok(s[20432] === 0, '恢复后回收站条目已清空');
    ok(checksumOk(s), '恢复后主库校验和正确');
    if (!tapUntil('B', () => screenText().includes('VAULT MENU'), 200, 6)) throw new Error('流程 F: 回收站返回菜单');
    menuGoto(0);
    if (!tapUntil('A', () => listReady(), 200, 6)) throw new Error('流程 F: 恢复后未进入列表');
    ok(rowText(6).includes('004 SHORT'), '恢复后列表 004 SHORT');
    fs.writeFileSync(path.join(OUT, 'save5-final.bin'), Buffer.from(sram()));

    /* --- v0.3.1 旧存档兼容: 设置区无效 (0x0D 有效位 0) 时取默认值 --- */
    {
        const sOld = Buffer.from(sram());
        sOld[13] = 0; sOld[14] = 0; sOld[15] = 0;
        let c = 0xA5;
        for (let i = 0; i < 16; i++) if (i !== 11) c ^= sOld[i];
        for (let i = 16; i < VAULT_BYTES; i++) c ^= sOld[i];
        sOld[11] = c & 0xFF;
        boot(sOld);
        if (!tapUntil('START', () => pinScreenReady('ENTER PIN', 6), 300, 8)) throw new Error('流程 F: v0.3.1 旧档未进入解锁');
        enterPin('123456');
        waitOk(() => screenText().includes('VAULT MENU'), '流程 F: v0.3.1 旧档解锁成功');
        menuGoto(5);
        if (!tapUntil('A', () => screenText().includes('SETTINGS') && settingsSel() === 0, 200, 6)) throw new Error('流程 F: v0.3.1 旧档未进入设置');
        frames(3);
        ok(rowText(4).includes('16'), 'v0.3.1 旧档默认生成长度 16');
        ok(rowText(6).includes('ON'), 'v0.3.1 旧档默认符号 ON');
        ok(rowText(8).includes('3 MIN'), 'v0.3.1 旧档默认锁屏 3 分钟');
        s = sram();
        ok(s[13] === 0 && s[14] === 0 && s[15] === 0, '读取旧档默认值不写设置区 (保持 0)');
        if (!tapUntil('B', () => screenText().includes('VAULT MENU'), 80, 6)) throw new Error('流程 F: v0.3.1 旧档设置返回失败');
        menuGoto(0);
        if (!tapUntil('A', () => listReady(), 200, 6)) throw new Error('流程 F: v0.3.1 旧档未进入列表');
        ok(rowText(16).includes('SEL=USER'), 'v0.3.1 旧档列表默认标签视图');
    }

    console.log('== 流程 G: 回收站单条永久删除 + START 清空 ==');
    {
        boot(buildSyntheticSave(3));
        if (!tapUntil('START', () => pinScreenReady('ENTER PIN', 6), 300, 8)) throw new Error('流程 G: 未进入解锁');
        enterPin('123456');
        waitOk(() => screenText().includes('VAULT MENU'), '流程 G: 解锁进入菜单');
        menuGoto(0);
        if (!tapUntil('A', () => listReady(), 200, 6)) throw new Error('流程 G: 未进入列表');

        listGoto(0);
        if (!tapUntil('A', () => screenText().includes(detailTitle(0)), 200, 6)) throw new Error('流程 G: 未打开 SLOT01');
        if (!tapUntil('START', () => screenText().includes('DELETE THIS SLOT?'), 200, 6)) throw new Error('流程 G: SLOT01 未出现删除确认');
        if (!tapUntil('A', () => listReady(), 600, 6)) throw new Error('流程 G: SLOT01 删除后未返回列表');

        listGoto(1);
        if (!tapUntil('A', () => screenText().includes(detailTitle(1)), 200, 6)) throw new Error('流程 G: 未打开 SLOT02');
        if (!tapUntil('START', () => screenText().includes('DELETE THIS SLOT?'), 200, 6)) throw new Error('流程 G: SLOT02 未出现删除确认');
        if (!tapUntil('A', () => listReady(), 600, 6)) throw new Error('流程 G: SLOT02 删除后未返回列表');
        s = sram();
        ok(recOf(s, 0).used === 0 && recOf(s, 1).used === 0, '流程 G: 两条记录已进入回收站');
        ok(s[20420] === 2, '流程 G: 回收站 head = 2');

        if (!tapUntil('B', () => screenText().includes('VAULT MENU'), 200, 6)) throw new Error('流程 G: 列表返回菜单失败');
        menuGoto(4);
        if (!tapUntil('A', () => rowText(0).includes('RECYCLE BIN'), 200, 6)) throw new Error('流程 G: 未进入回收站');
        frames(5);
        ok(rowText(1).includes('SLOT02') && rowText(2).includes('SLOT01'), '流程 G: 回收站按最近删除排序');

        if (!tapUntil('SELECT', () => screenText().includes('DELETE THIS ITEM?'), 200, 6)) throw new Error('流程 G: SELECT 未进入永久删除确认');
        if (!tapUntil('A', () => rowText(1).includes('SLOT01') && !rowText(1).includes('SLOT02'), 500, 6)) throw new Error('流程 G: 永久删除 SLOT02 失败');
        s = sram();
        ok(s[20420] === 2, '流程 G: 单条删除不改变 head');
        ok(s[20432 + REC_SZ] === 0, '流程 G: SLOT02 回收站记录已清零');
        ok(binChecksumOk(s), '流程 G: 单条删除后回收站校验和正确');

        if (!tapUntil('START', () => screenText().includes('EMPTY RECYCLE BIN?'), 200, 6)) throw new Error('流程 G: START 未进入清空确认');
        if (!tapUntil('A', () => screenText().includes('BIN EMPTY'), 600, 6)) throw new Error('流程 G: 清空回收站失败');
        s = sram();
        ok(s[20420] === 0, '流程 G: 清空后 head = 0');
        for (let i = 0; i < 16 * REC_SZ; i++) {
            if (s[20432 + i] !== 0) { ok(false, '流程 G: 清空后所有回收站记录为 0'); break; }
            if (i === 16 * REC_SZ - 1) ok(true, '流程 G: 清空后所有回收站记录为 0');
        }
        ok(binChecksumOk(s), '流程 G: 清空后回收站校验和正确');
        ok(checksumOk(s), '流程 G: 回收站操作不影响主库校验和');
    }
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
