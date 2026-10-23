/*
 * GB Password Vault v0.3
 * ------------------------------------------------------------------
 * 在真实 Game Boy (DMG / GBC 兼容模式 / GBA) 上运行的离线密码管理器。
 * 编译工具链: GBDK-2020 4.5.0  (C/SDCC)
 * 卡带类型:   MBC1 + 32KB SRAM (4 x 8KB, RAM banking) + BATTERY  (标题 PASSMGR)
 *
 * 功能:
 *   - 首次开机: 设置 4 位数字 PIN (SRAM 中以 +7 取模混淆存储)
 *   - 开机解锁: 3x4 数字键盘输入 PIN, 错误 3 次进入 SYSTEM LOCKED 彩蛋
 *   - 主菜单:   查看账号 / 添加账号 / 修改 PIN / 锁库
 *   - 账号:     255 个槽位, 每槽 8 字符标签 + 36 字符密码
 *   - 添加流程: 屏幕虚拟键盘 (大写/小写/符号页三种模式, D-pad 选字 A 输入);
 *              密码页按 SELECT 生成 16 位随机强密码 (再按重掷, 可继续手改)
 *   - 列表页:   12 行滚动窗口 + 右上角位置 (nnn/255), 上下步进 / 左右跳 8 条
 *   - 详情页:   按住 A 显示密码明文 (分两行), START 删除该槽位
 *   - 全部数据即时写入 SRAM (带全局校验和), 关机不丢
 *
 * SRAM 布局 (平面地址; MBC1 模式 1 下经 0x4000 切 8KB 存储体):
 *   0x0000  MAGIC  "PMG3"
 *   0x0004  PIN    4 字节混淆值 ((digit+7) % 10)
 *   0x0008  TRIES  连续错误次数 (达到 3 进入锁定)
 *   0x0009  CHK    全局校验和 (前面各字节 + 槽位区 XOR, 种子 0xA5)
 *   0x000A  FLAGS  bit0 = 已设置 PIN
 *   0x0010  槽位区 255 x 47 字节:
 *          [0] used(0xA5) [1] llen [2..9] label [10] plen [11..46] pass
 *   总长 16 + 255*47 = 12001 字节, 跨存储体 0 与 1 (卡带需 32KB SRAM)
 */
#include <gb/gb.h>
#include <stdint.h>
#include <string.h>

#include "font_data.h"

#define N_SLOTS   255
#define LBL_MAX   8
#define PSS_MAX  36
#define REC_SZ   (2 + LBL_MAX + 1 + PSS_MAX)   /* used+llen+label+plen+pass = 47 */

#define USED_MAGIC 0xA5
#define FLAG_PINSET 0x01

#define OFS_MAGIC  0
#define OFS_PIN    4
#define OFS_TRIES  8
#define OFS_CHK    9
#define OFS_FLAGS 10
#define OFS_SLOTS 16

#define SR ((volatile uint8_t *)0xA000)

/* 平面地址: 头部 16 字节 + 255*47 = 12001 字节 */
#define REC_ADDR(i)  ((uint16_t)(OFS_SLOTS + (uint16_t)(i) * REC_SZ))
#define VAULT_BYTES  ((uint16_t)(OFS_SLOTS + (uint16_t)N_SLOTS * REC_SZ))

/* MBC1 寄存器: 模式 1 时 0x4000 = RAM 存储体选择, 0x6000 = 模式寄存器 */
#define RAMBANK_REG (*(volatile uint8_t *)0x4000)
#define RAMMODE_REG (*(volatile uint8_t *)0x6000)

typedef struct {
    uint8_t used;
    uint8_t llen;
    char    label[LBL_MAX + 1];
    uint8_t plen;
    char    pass[PSS_MAX + 1];
} slot_t;

static slot_t  cur;             /* 详情页当前槽位缓存 */
static uint8_t pin_obf[4];
static uint8_t pin_set;
static uint8_t tries;

static const uint8_t MAGIC[4] = { 'P', 'M', 'G', '3' };

/* ------------------------------------------------------------------ */
/* SRAM 访问 (MBC1 32KB = 4 x 8KB, 平面地址自动切存储体)               */
/* ------------------------------------------------------------------ */

static uint8_t cur_bank = 0xFF;   /* 当前存储体缓存 (0xFF = 未知) */

static void sram_on(void)  { *(volatile uint8_t *)0x0000 = 0x0A; }
static void sram_off(void) { *(volatile uint8_t *)0x0000 = 0x00; }

static void sram_hw_init(void)
{
    RAMMODE_REG = 0x01;      /* MBC1 模式 1: 0x4000 = RAM 存储体 */
    cur_bank = 0xFF;         /* 强制首次访问重写存储体寄存器 */
}

static void bank_set(uint8_t b)
{
    if (b != cur_bank) { RAMBANK_REG = b; cur_bank = b; }
}

/* 平面地址读/写: 每 8KB 一档自动切体, 地址范围 0..12000 */
static uint8_t rb(uint16_t a)
{
    bank_set((uint8_t)(a >> 13));
    return SR[a & 0x1FFF];
}

static void wb(uint16_t a, uint8_t v)
{
    bank_set((uint8_t)(a >> 13));
    SR[a & 0x1FFF] = v;
}

static uint8_t chk_calc(void)
{
    uint8_t c = 0xA5;
    uint16_t i;
    for (i = 0; i < OFS_SLOTS; i++) {
        if (i != OFS_CHK) c ^= rb(i);
    }
    for (i = OFS_SLOTS; i < VAULT_BYTES; i++) {
        c ^= rb(i);
    }
    return c;
}

/* 写入头部 (MAGIC/PIN/TRIES/FLAGS) 并重算全局校验和 */
static void header_save(void)
{
    uint8_t i;
    sram_on();
    for (i = 0; i < 4; i++) wb(OFS_MAGIC + i, MAGIC[i]);
    for (i = 0; i < 4; i++) wb(OFS_PIN + i, pin_obf[i]);
    wb(OFS_TRIES, tries);
    wb(OFS_FLAGS, pin_set ? FLAG_PINSET : 0x00);
    wb(OFS_CHK, chk_calc());
    sram_off();
}

/* 记录改动提交: 重算并写入全局校验和 */
static void vault_commit(void)
{
    sram_on();
    wb(OFS_CHK, chk_calc());
    sram_off();
}

/* 读入头部; 校验失败返回 0 (视为空库, 交由上层重新设置) */
static uint8_t vault_load(void)
{
    uint8_t i, ok = 1;

    sram_on();
    for (i = 0; i < 4; i++) {
        if (rb(OFS_MAGIC + i) != MAGIC[i]) { ok = 0; break; }
    }
    if (ok && rb(OFS_CHK) != chk_calc()) ok = 0;

    if (ok) {
        for (i = 0; i < 4; i++) pin_obf[i] = rb(OFS_PIN + i);
        tries = rb(OFS_TRIES);
        if (tries > 3) tries = 3;
        pin_set = (rb(OFS_FLAGS) & FLAG_PINSET) ? 1 : 0;
    }
    sram_off();

    if (!ok) {
        pin_set = 0;
        tries = 0;
        for (i = 0; i < 4; i++) pin_obf[i] = 0;
    }
    return ok;
}

/* 槽位区整体清零 (清库/首次设置; 不碰头部) */
static void vault_clear_slots(void)
{
    uint16_t i;
    sram_on();
    for (i = OFS_SLOTS; i < VAULT_BYTES; i++) wb(i, 0x00);
    sram_off();
}

/* SYSTEM LOCKED 彩蛋: 擦除全部数据 */
static void vault_wipe(void)
{
    uint8_t i;
    for (i = 0; i < 4; i++) pin_obf[i] = 0;
    pin_set = 0;
    tries = 0;
    vault_clear_slots();
    header_save();
}

/* 第一个空槽 (无则 -1) */
static int16_t find_empty(void)
{
    uint16_t i;
    sram_on();
    for (i = 0; i < N_SLOTS; i++) {
        if (rb(REC_ADDR(i)) != USED_MAGIC) { sram_off(); return (int16_t)i; }
    }
    sram_off();
    return -1;
}

static uint8_t slot_used(uint16_t i)
{
    uint8_t u;
    sram_on();
    u = (uint8_t)(rb(REC_ADDR(i)) == USED_MAGIC);
    sram_off();
    return u;
}

static void slot_read(uint16_t i, slot_t *s)
{
    uint16_t a = REC_ADDR(i);
    uint8_t j;

    memset(s, 0, sizeof(slot_t));
    sram_on();
    if (rb(a) != USED_MAGIC) { sram_off(); return; }
    s->used = 1;
    s->llen = rb((uint16_t)(a + 1));
    if (s->llen > LBL_MAX) s->llen = LBL_MAX;
    for (j = 0; j < LBL_MAX; j++) s->label[j] = (char)rb((uint16_t)(a + 2 + j));
    s->label[LBL_MAX] = 0;
    s->plen = rb((uint16_t)(a + 10));
    if (s->plen > PSS_MAX) s->plen = PSS_MAX;
    for (j = 0; j < PSS_MAX; j++) s->pass[j] = (char)rb((uint16_t)(a + 11 + j));
    s->pass[PSS_MAX] = 0;
    sram_off();
}

static void slot_write(uint16_t i, const slot_t *s)
{
    uint16_t a = REC_ADDR(i);
    uint8_t j;

    sram_on();
    wb(a, s->used ? USED_MAGIC : 0x00);
    wb((uint16_t)(a + 1), s->llen);
    for (j = 0; j < LBL_MAX; j++) wb((uint16_t)(a + 2 + j), (uint8_t)s->label[j]);
    wb((uint16_t)(a + 10), s->plen);
    for (j = 0; j < PSS_MAX; j++) wb((uint16_t)(a + 11 + j), (uint8_t)s->pass[j]);
    sram_off();
}

/* 整条记录清零 (删除; 不留旧密码残迹) */
static void slot_clear(uint16_t i)
{
    uint16_t a = REC_ADDR(i);
    uint8_t j;

    sram_on();
    for (j = 0; j < REC_SZ; j++) wb((uint16_t)(a + j), 0x00);
    sram_off();
}

/* ------------------------------------------------------------------ */
/* 输入                                                                */
/* ------------------------------------------------------------------ */

/* 等待单次按键: waitpad 在按住期间会反复返回, 必须配 waitpadup,
 * 保证一次物理按下只产生一次输入 (真机连发防护)。 */
static uint8_t getkey(uint8_t mask)
{
    uint8_t k = waitpad(mask);
    waitpadup();
    return k;
}

/* ------------------------------------------------------------------ */
/* 屏幕绘制工具                                                        */
/* ------------------------------------------------------------------ */

/* 字符 -> tile id: 0x20..0x7F 映射 0..95 (正常), 反白 = +96 (96..191); 未知按空格 */
static uint8_t tile_of(char c)
{
    uint8_t u = (uint8_t)c;
    if (u < 0x20 || u > 0x7F) u = ' ';
    return (uint8_t)(u - 0x20);
}

static void put_str(uint8_t x, uint8_t y, const char *s)
{
    uint8_t buf[20];
    uint8_t n = 0;
    while (s[n] && n < (uint8_t)(20 - x)) {
        buf[n] = tile_of(s[n]);
        n++;
    }
    if (n) set_bkg_tiles(x, y, n, 1, buf);
}

/* 固定宽度绘制, inv=1 时使用反白 tile (tile id + 96), NUL 之后补空格 */
static void put_fixed(uint8_t x, uint8_t y, const char *s, uint8_t width, uint8_t inv)
{
    uint8_t buf[20];
    uint8_t i, t, done = 0;
    for (i = 0; i < width && i < 20; i++) {
        if (!done && s[i] == 0) done = 1;
        t = done ? 0 : tile_of(s[i]);
        buf[i] = inv ? (uint8_t)(t + 96) : t;
    }
    set_bkg_tiles(x, y, width, 1, buf);
}

static void screen_clear(void)
{
    uint8_t row[20];
    uint8_t y;
    memset(row, 0, 20);
    for (y = 0; y < 18; y++) set_bkg_tiles(0, y, 20, 1, row);
}

static void toast(const char *msg)
{
    put_str(1, 15, msg);
    delay(900);
}

/* v (0..999) 写成 3 位零填充十进制 + NUL */
static void fmt3_zp(char *out, uint16_t v)
{
    out[0] = (char)('0' + (v / 100) % 10);
    out[1] = (char)('0' + (v / 10) % 10);
    out[2] = (char)('0' + v % 10);
    out[3] = 0;
}

/* 列表右上角位置 "001/255" (7 字符 + NUL) */
static void fmt_pos(char *out, uint16_t sel1)
{
    fmt3_zp(out, sel1);
    out[3] = '/';
    fmt3_zp(out + 4, (uint16_t)N_SLOTS);   /* 在 out[7] 写 NUL */
}

/* 详情页标题 "SLOT 001/255" (12 字符 + NUL) */
static void fmt_slot_title(char *t, uint16_t idx1)
{
    t[0] = 'S'; t[1] = 'L'; t[2] = 'O'; t[3] = 'T'; t[4] = ' ';
    fmt3_zp(t + 5, idx1);
    t[8] = '/';
    fmt3_zp(t + 9, (uint16_t)N_SLOTS);     /* 在 t[12] 写 NUL */
}

/* ------------------------------------------------------------------ */
/* 随机密码生成器 (熵: 按键时机 tick + DIV/LY 混入 16 位 LCG)          */
/* ------------------------------------------------------------------ */

#define GEN_LEN 16

/* 避开易混字符: 无 0 O 1 l I */
static const char GEN_UP[25]  = "ABCDEFGHJKLMNPQRSTUVWXYZ";    /* 24 */
static const char GEN_LO[26]  = "abcdefghijkmnopqrstuvwxyz";   /* 25 */
static const char GEN_DIG[9]  = "23456789";                    /* 8  */
static const char GEN_SYM[14] = ".-_@!#$%&()?+";               /* 13 */
static const char GEN_ALL[71] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789.-_@!#$%&()?+"; /* 70 */

static uint16_t rng_state = 0xACE1;

static uint8_t rng_byte(void)
{
    rng_state = (uint16_t)(rng_state * 251u + 33u);
    return (uint8_t)(rng_state >> 8);
}

static void rng_mix(uint16_t v)
{
    rng_state ^= v;
    rng_state = (uint16_t)(rng_state * 251u + 33u);
    rng_state ^= (uint16_t)(DIV_REG | ((uint16_t)LY_REG << 8));
    rng_state = (uint16_t)(rng_state * 251u + 33u);
}

/* 生成 n 位强密码 (默认 16): 四类字符各保一 + 随机填充 + 洗牌 */
static uint8_t gen_password(char *buf, uint8_t max)
{
    uint8_t n = (max < GEN_LEN) ? max : GEN_LEN;
    uint8_t i, j, t;

    buf[0] = GEN_UP[rng_byte() % 24];
    buf[1] = GEN_LO[rng_byte() % 25];
    buf[2] = GEN_DIG[rng_byte() % 8];
    buf[3] = GEN_SYM[rng_byte() % 13];
    for (i = 4; i < n; i++) buf[i] = GEN_ALL[rng_byte() % 70];
    for (i = (uint8_t)(n - 1); i > 0; i--) {          /* Fisher-Yates */
        j = (uint8_t)(rng_byte() % (uint8_t)(i + 1));
        t = (uint8_t)buf[i];
        buf[i] = buf[j];
        buf[j] = (char)t;
    }
    buf[n] = 0;
    return n;
}

/* ------------------------------------------------------------------ */
/* 数字键盘 (PIN 输入)                                                 */
/* ------------------------------------------------------------------ */

static const uint8_t PAD_X[3] = { 6, 10, 14 };
static const uint8_t PAD_Y[4] = { 6, 8, 10, 12 };
static const char *PAD_TXT[12] = {
    " 1 ", " 2 ", " 3 ",
    " 4 ", " 5 ", " 6 ",
    " 7 ", " 8 ", " 9 ",
    "DEL", " 0 ", " OK"
};

static void pad_draw_cell(uint8_t cx, uint8_t cy, uint8_t inv)
{
    uint8_t idx = (uint8_t)(cy * 3 + cx);
    put_fixed(PAD_X[cx], PAD_Y[cy], PAD_TXT[idx], 3, inv);
}

static void pad_draw_field(const uint8_t *out, uint8_t n)
{
    char line[5];
    uint8_t i;
    for (i = 0; i < 4; i++) {
        line[i] = (i < n) ? (char)('0' + out[i]) : '_';
    }
    line[4] = 0;
    put_str(12, 3, line);
}

/* 输入 4 位 PIN, 集满 4 位或按 OK/START 返回 */
static void pin_entry(const char *title, uint8_t *out)
{
    uint8_t n = 0, cx = 0, cy = 0, k;

    screen_clear();
    put_str(6, 1, title);
    put_str(6, 3, "PIN:");
    pad_draw_field(out, n);
    {
        uint8_t r, c;
        for (r = 0; r < 4; r++)
            for (c = 0; c < 3; c++)
                pad_draw_cell(c, r, 0);
    }
    put_str(1, 15, "A=INPUT B=DEL");
    put_str(1, 16, "START=OK");
    pad_draw_cell(cx, cy, 1);

    while (1) {
        k = getkey(J_UP | J_DOWN | J_LEFT | J_RIGHT | J_A | J_B | J_START);

        if (k & J_UP)    { if (cy) { pad_draw_cell(cx, cy, 0); cy--; pad_draw_cell(cx, cy, 1); } continue; }
        if (k & J_DOWN)  { if (cy < 3) { pad_draw_cell(cx, cy, 0); cy++; pad_draw_cell(cx, cy, 1); } continue; }
        if (k & J_LEFT)  { if (cx) { pad_draw_cell(cx, cy, 0); cx--; pad_draw_cell(cx, cy, 1); } continue; }
        if (k & J_RIGHT) { if (cx < 2) { pad_draw_cell(cx, cy, 0); cx++; pad_draw_cell(cx, cy, 1); } continue; }

        if (k & J_B) {
            if (n) { n--; pad_draw_field(out, n); }
            continue;
        }
        if (k & J_A) {
            uint8_t idx = (uint8_t)(cy * 3 + cx);
            if (idx <= 8) {                     /* 1..9 */
                if (n < 4) { out[n] = (uint8_t)(idx + 1); n++; pad_draw_field(out, n); }
            } else if (idx == 9) {              /* DEL */
                if (n) { n--; pad_draw_field(out, n); }
            } else if (idx == 10) {             /* 0 */
                if (n < 4) { out[n] = 0; n++; pad_draw_field(out, n); }
            } else {                            /* OK */
                if (n == 4) return;
            }
            if (n == 4) return;
            continue;
        }
        if (k & J_START) {
            if (n == 4) return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 虚拟键盘 (文本输入)                                                 */
/*   三种模式: 大写 / 小写 / 符号页                                    */
/*   5x8 网格 + 底行 5 键: Aa(大小写) SYM/ABC(符号页) SPACE DEL OK     */
/* ------------------------------------------------------------------ */

#define KB_UPPER 0
#define KB_LOWER 1
#define KB_SYM   2

static const char KB_UP[41] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_@";
static const char KB_LO[41] = "abcdefghijklmnopqrstuvwxyz0123456789.-_@";
static const char *const KB_SYM_ROWS[4] = {
    "!\"#$%&'(",
    ")*+,-./:",
    ";<=>?@[\\",
    "]^_`{|}~"
};

static const uint8_t KX5[5] = { 0, 3, 7, 14, 18 };   /* 底行 x 坐标 */
static const uint8_t KW5[5] = { 2, 3, 6, 3, 2 };      /* 底行宽度   */

static uint8_t kb_mode = KB_UPPER;

static uint8_t kb_y(uint8_t row) { return (uint8_t)(4 + row * 2); }
static uint8_t kb_row_max(void) { return (kb_mode == KB_SYM) ? 3 : 4; }
static uint8_t kb_col_max(uint8_t row) { return (row < 5) ? 7 : 4; }

static char kb_char(uint8_t row, uint8_t col)
{
    if (kb_mode == KB_SYM) {
        return (row < 4) ? KB_SYM_ROWS[row][col] : ' ';
    }
    return (kb_mode == KB_LOWER ? KB_LO : KB_UP)[row * 8 + col];
}

/* 15 格输入窗: 超过 14 字符时向左滚动, 光标 '_' 恒定在可见末尾 */
static void draw_field_line(const char *buf, uint8_t len)
{
    char line[16];
    uint8_t start = (len > 14) ? (uint8_t)(len - 14) : 0;
    uint8_t i;
    for (i = 0; i < 15; i++) {
        uint8_t idx = (uint8_t)(start + i);
        line[i] = (idx < len) ? buf[idx] : (char)((idx == len) ? '_' : ' ');
    }
    line[15] = 0;
    put_str(1, 2, line);
}

static void kb_draw_cell(uint8_t row, uint8_t col, uint8_t inv)
{
    char tmp[2];
    if (row < 5) {
        tmp[0] = kb_char(row, col);
        tmp[1] = 0;
        put_fixed((uint8_t)(2 + col * 2), kb_y(row), tmp, 1, inv);
    } else {
        const char *lbl;
        if      (col == 0) lbl = "Aa";
        else if (col == 1) lbl = (kb_mode == KB_SYM) ? "ABC" : "SYM";
        else if (col == 2) lbl = "SPACE";
        else if (col == 3) lbl = "DEL";
        else               lbl = "OK";
        put_fixed(KX5[col], kb_y(5), lbl, KW5[col], inv);
    }
}

static void draw_edit(const char *title, const char *buf, uint8_t len,
                      uint8_t row, uint8_t col, uint8_t gen)
{
    uint8_t r, c;
    screen_clear();
    put_str(1, 1, title);
    draw_field_line(buf, len);
    for (r = 0; r < 5; r++)
        for (c = 0; c < 8; c++)
            kb_draw_cell(r, c, 0);
    for (c = 0; c < 5; c++)
        kb_draw_cell(5, c, 0);
    put_str(1, 16, "A=ADD  B=DEL");
    put_str(1, 17, gen ? "START=OK SEL=GEN" : "START=OK SEL=ESC");
    kb_draw_cell(row, col, 1);
}

/* 返回输入长度; gen=0 时 SELECT = 取消(-1); gen=1 时 SELECT = 生成随机密码 */
static int16_t edit_field(const char *title, char *buf, uint8_t max, uint8_t gen)
{
    uint8_t len = 0, row = 0, col = 0, k;
    uint16_t tick = 0;

    kb_mode = KB_UPPER;
    buf[0] = 0;
    draw_edit(title, buf, len, row, col, gen);

    while (1) {
        tick++;
        k = getkey(J_UP | J_DOWN | J_LEFT | J_RIGHT | J_A | J_B | J_START | J_SELECT);
        rng_mix((uint16_t)(tick + ((uint16_t)k << 8)));

        if (k & J_SELECT) {
            if (gen) {                  /* 生成 / 重掷随机密码 */
                len = gen_password(buf, max);
                draw_field_line(buf, len);
                continue;
            }
            return -1;
        }
        if (k & J_START) {
            if (len) return (int16_t)len;
            continue;
        }
        if (k & J_B) {
            if (len) { len--; buf[len] = 0; draw_field_line(buf, len); }
            continue;
        }
        if (k & J_LEFT) {
            if (col) { kb_draw_cell(row, col, 0); col--; kb_draw_cell(row, col, 1); }
            continue;
        }
        if (k & J_RIGHT) {
            if (col < kb_col_max(row)) { kb_draw_cell(row, col, 0); col++; kb_draw_cell(row, col, 1); }
            continue;
        }
        if (k & J_UP) {
            if (row == 5) {
                kb_draw_cell(5, col, 0);
                row = kb_row_max();
                if (col > kb_col_max(row)) col = kb_col_max(row);
                kb_draw_cell(row, col, 1);
            } else if (row) {
                kb_draw_cell(row, col, 0);
                row--;
                if (col > kb_col_max(row)) col = kb_col_max(row);
                kb_draw_cell(row, col, 1);
            }
            continue;
        }
        if (k & J_DOWN) {
            if (row == kb_row_max()) {
                kb_draw_cell(row, col, 0);
                row = 5;
                if (col > 4) col = 4;
                kb_draw_cell(5, col, 1);
            } else if (row < 5) {
                kb_draw_cell(row, col, 0);
                row++;
                if (col > kb_col_max(row)) col = kb_col_max(row);
                kb_draw_cell(row, col, 1);
            }
            continue;
        }
        if (k & J_A) {
            if (row < 5) {
                char ch = kb_char(row, col);
                if (ch != ' ' && len < max) {
                    buf[len] = ch; len++; buf[len] = 0;
                    draw_field_line(buf, len);
                }
            } else if (col == 0) {          /* Aa: 切大小写 (符号页则回大写) */
                kb_mode = (kb_mode == KB_SYM) ? KB_UPPER
                        : ((kb_mode == KB_UPPER) ? KB_LOWER : KB_UPPER);
                draw_edit(title, buf, len, row, col, gen);
            } else if (col == 1) {          /* SYM / ABC: 切符号页 */
                kb_mode = (kb_mode == KB_SYM) ? KB_UPPER : KB_SYM;
                draw_edit(title, buf, len, row, col, gen);
            } else if (col == 2) {          /* SPACE */
                if (len < max) { buf[len] = ' '; len++; buf[len] = 0; draw_field_line(buf, len); }
            } else if (col == 3) {          /* DEL */
                if (len) { len--; buf[len] = 0; draw_field_line(buf, len); }
            } else {                        /* OK */
                if (len) return (int16_t)len;
            }
            continue;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 槽位列表 (12 行滚动窗口) / 详情 / 添加                              */
/* ------------------------------------------------------------------ */

#define LIST_ROWS 12
#define LIST_TOP  3

static uint16_t list_sel = 0;    /* 全局选中槽位 (0..254) */
static uint16_t list_view = 0;   /* 窗口首行槽位 */

/* 保证选中项在窗口内可见 (边缘滚动) */
static void list_ensure_visible(void)
{
    if (list_sel < list_view) {
        list_view = list_sel;
    } else if (list_sel >= (uint16_t)(list_view + LIST_ROWS)) {
        list_view = (uint16_t)(list_sel + 1 - LIST_ROWS);
    }
}

/* "001 EMAIL   " (12 字符): 3 位零填充编号 + 空格 + 8 字符标签/EMPTY */
static void slot_row_text(uint16_t idx, char *out)
{
    slot_t s;
    uint8_t j;

    slot_read(idx, &s);
    fmt3_zp(out, (uint16_t)(idx + 1));
    out[3] = ' ';
    if (s.used) {
        for (j = 0; j < LBL_MAX; j++) out[4 + j] = s.label[j] ? s.label[j] : ' ';
    } else {
        static const char *e = "EMPTY";
        for (j = 0; j < LBL_MAX; j++) out[4 + j] = e[j] ? e[j] : ' ';
    }
    out[12] = 0;
}

static void draw_slot_list(const char *title)
{
    char row[13], pos[8];
    uint8_t r;
    uint16_t idx;

    screen_clear();
    put_str(1, 0, title);
    fmt_pos(pos, (uint16_t)(list_sel + 1));
    put_str(13, 0, pos);

    for (r = 0; r < LIST_ROWS; r++) {
        idx = (uint16_t)(list_view + r);
        if (idx < N_SLOTS) {
            slot_row_text(idx, row);
            if (idx == list_sel) put_fixed(1, (uint8_t)(LIST_TOP + r), row, 12, 1);
            else                 put_str(1, (uint8_t)(LIST_TOP + r), row);
        } else {
            put_fixed(1, (uint8_t)(LIST_TOP + r), "", 12, 0);
        }
    }
    put_str(1, 16, "L/R=JUMP 8");
    put_str(1, 17, "A=OPEN B=BACK");
}

static void draw_pass_line(uint8_t reveal)
{
    char line[21];
    uint8_t j, off, n = cur.plen;
    for (off = 0; off < 2; off++) {
        for (j = 0; j < 20; j++) {
            uint8_t idx = (uint8_t)(off * 20 + j);
            line[j] = (idx < n) ? (reveal ? cur.pass[idx] : '*') : ' ';
        }
        line[20] = 0;
        put_str(0, (uint8_t)(5 + off), line);
    }
}

static void draw_detail(uint16_t i)
{
    char t[13];
    screen_clear();
    fmt_slot_title(t, (uint16_t)(i + 1));
    put_str(1, 0, t);
    put_str(1, 2, "LABEL:");
    put_fixed(8, 2, cur.label, LBL_MAX, 0);
    put_str(1, 4, "PASS:");
    draw_pass_line(0);
    put_str(1, 16, "A=REVEAL B=BACK");
    put_str(1, 17, "START=DELETE");
}

static void detail_screen(uint16_t i)
{
    uint8_t prev = 0, reveal = 0, pressed, k;

    slot_read(i, &cur);
    if (!cur.used) return;

    draw_detail(i);
    /* 纯 joypad 忙轮询 (每帧数千次): 不依赖 HALT/中断帧同步,
     * 否则模拟器 (serverboy) 的 HALT 唤醒会被按键事件打断, 点按会丢。 */
    while (1) {
        uint8_t j = joypad();
        pressed = (uint8_t)(j & ~prev);

        if ((j & J_A) && !reveal) { reveal = 1; draw_pass_line(1); }
        else if (!(j & J_A) && reveal) { reveal = 0; draw_pass_line(0); }

        if (pressed & J_B) { waitpadup(); return; }

        if (pressed & J_START) {
            put_str(1, 13, "DELETE THIS SLOT?");
            put_str(1, 14, "A=YES  B=NO");
            waitpadup();
            k = getkey(J_A | J_B);
            if (k & J_A) {
                slot_clear(i);
                vault_commit();
                toast("DELETED");
                waitpadup();
                return;
            }
            draw_detail(i);
            reveal = 0;
        }
        prev = j;
    }
}

static void add_flow(int16_t slot)
{
    char lab[LBL_MAX + 1];
    char pw[PSS_MAX + 1];
    slot_t s;
    int16_t r;

    if (slot < 0) {                 /* 自动写入第一个空槽 */
        slot = find_empty();
        if (slot < 0) { toast("VAULT FULL"); return; }
    }

    screen_clear();
    r = edit_field("LABEL (MAX 8)", lab, LBL_MAX, 0);
    if (r < 0) return;

    screen_clear();
    r = edit_field("PASSWORD (MAX 36)", pw, PSS_MAX, 1);
    if (r < 0) return;

    memset(&s, 0, sizeof(slot_t));
    s.used = 1;
    s.llen = (uint8_t)strlen(lab);
    memcpy(s.label, lab, s.llen);
    s.plen = (uint8_t)strlen(pw);
    memcpy(s.pass, pw, s.plen);

    slot_write((uint16_t)slot, &s);
    vault_commit();
    toast("SAVED");
}

static void list_screen(void)
{
    uint8_t k;

    list_ensure_visible();
    while (1) {
        draw_slot_list("ACCOUNTS");
        k = getkey(J_UP | J_DOWN | J_LEFT | J_RIGHT | J_A | J_B | J_START);
        if (k & J_UP) {
            if (list_sel) list_sel--;
            list_ensure_visible();
            continue;
        }
        if (k & J_DOWN) {
            if (list_sel < (uint16_t)(N_SLOTS - 1)) list_sel++;
            list_ensure_visible();
            continue;
        }
        if (k & J_LEFT) {           /* 向左跳 8 条 */
            list_sel = (list_sel > 8) ? (uint16_t)(list_sel - 8) : 0;
            list_ensure_visible();
            continue;
        }
        if (k & J_RIGHT) {          /* 向右跳 8 条 */
            list_sel = (uint16_t)((list_sel + 8 <= N_SLOTS - 1) ? list_sel + 8 : N_SLOTS - 1);
            list_ensure_visible();
            continue;
        }
        if ((k & J_B) || (k & J_START)) return;
        if (k & J_A) {
            if (slot_used(list_sel)) detail_screen(list_sel);
            else add_flow((int16_t)list_sel);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 解锁 / 初始化 / 改 PIN / 锁定彩蛋                                    */
/* ------------------------------------------------------------------ */

static void locked_screen(void)
{
    uint8_t wraps = 0;
    uint8_t prev_tima;

    screen_clear();
    put_str(2, 4, "SYSTEM LOCKED");
    put_str(2, 6, "VAULT SEALED");
    put_str(1, 9, "HOLD A+B+SEL+START");
    put_str(1, 10, "3 SECONDS TO WIPE");
    put_str(1, 12, "OR POWER OFF");
    put_str(1, 15, "RECOVERY ERASES ALL");
    /* 纯 joypad 忙轮询, 不依赖 HALT/中断/画面采样。
     * 长按计时: 硬件定时器 TIMA @4096Hz (每 1024 周期 +1, 真机与
     * 模拟器周期一致), 8 位回绕一次 = 256 tick ≈ 62.5ms,
     * 48 次回绕 = 正好 3 秒; 松开任一键立即清零重计。 */
    TAC_REG = TACF_START | TACF_4KHZ;
    TIMA_REG = 0;
    prev_tima = 0;

    while (1) {
        uint8_t j = joypad();
        uint8_t t = TIMA_REG;

        if ((j & (J_A | J_B | J_SELECT | J_START)) == (J_A | J_B | J_SELECT | J_START)) {
            if (t < prev_tima) {            /* TIMA 8 位回绕 = 256 tick */
                if (++wraps >= 48) {
                    TAC_REG = TACF_STOP;    /* 先关定时器再擦除 */
                    vault_wipe();
                    toast("VAULT WIPED");
                    return;
                }
            }
        } else {
            wraps = 0;
        }
        prev_tima = t;
    }
}

static uint8_t pin_ok(const uint8_t *in)
{
    uint8_t i;
    for (i = 0; i < 4; i++) {
        if (((in[i] + 7) % 10) != pin_obf[i]) return 0;
    }
    return 1;
}

static uint8_t unlock(void)
{
    uint8_t in[4];

    if (tries >= 3) {
        locked_screen();
        return 0;
    }
    while (1) {
        pin_entry("ENTER PIN", in);
        if (pin_ok(in)) {
            if (tries) { tries = 0; header_save(); }
            return 1;
        }
        tries++;
        header_save();
        if (tries >= 3) {
            locked_screen();
            return 0;
        }
        toast("WRONG PIN");
    }
}

static void setup_pin(void)
{
    uint8_t a[4], b[4], i;
    uint8_t same;

    screen_clear();
    put_str(2, 1, "FIRST TIME SETUP");
    put_str(1, 4, "CHOOSE 4-DIGIT PIN");
    put_str(1, 5, "IT UNLOCKS THE VAULT");
    delay(1500);

    while (1) {
        pin_entry("NEW PIN", a);
        pin_entry("CONFIRM PIN", b);
        same = 1;
        for (i = 0; i < 4; i++) {
            if (a[i] != b[i]) { same = 0; break; }
        }
        if (same) break;
        toast("PIN MISMATCH");
    }

    for (i = 0; i < 4; i++) pin_obf[i] = (uint8_t)((a[i] + 7) % 10);
    pin_set = 1;
    tries = 0;
    vault_clear_slots();            /* 首次设置: 清空全部槽位 */
    header_save();
    toast("PIN SAVED");
}

static void change_pin(void)
{
    uint8_t a[4], b[4], i, same;

    while (1) {
        pin_entry("NEW PIN", a);
        pin_entry("CONFIRM PIN", b);
        same = 1;
        for (i = 0; i < 4; i++) {
            if (a[i] != b[i]) { same = 0; break; }
        }
        if (same) break;
        toast("PIN MISMATCH");
    }
    for (i = 0; i < 4; i++) pin_obf[i] = (uint8_t)((a[i] + 7) % 10);
    header_save();
    toast("PIN UPDATED");
}

static void menu_screen(void)
{
    static const char *items[4] = {
        "VIEW ACCOUNTS", "ADD ACCOUNT", "CHANGE PIN", "LOCK VAULT"
    };
    uint8_t sel = 0, k, i;

    while (1) {
        screen_clear();
        put_str(5, 1, "VAULT MENU");
        for (i = 0; i < 4; i++) {
            if (i == sel) put_fixed(2, (uint8_t)(6 + i * 2), items[i], 13, 1);
            else          put_str(2, (uint8_t)(6 + i * 2), items[i]);
        }
        put_str(1, 15, "A=SELECT");
        put_str(1, 17, "PRESS A TO OPEN");
        k = getkey(J_UP | J_DOWN | J_A);
        if (k & J_UP)   { if (sel) sel--; continue; }
        if (k & J_DOWN) { if (sel < 3) sel++; continue; }
        if (k & J_A) {
            if      (sel == 0) list_screen();
            else if (sel == 1) add_flow(-1);
            else if (sel == 2) change_pin();
            else return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 入口                                                                */
/* ------------------------------------------------------------------ */

void main(void)
{
    DISPLAY_OFF;
    set_bkg_data(0, 192, font_tiles);
    *(volatile uint8_t *)0xFF47 = 0xFC;      /* BGP: 0=白 1=黑 (DMG) */
    screen_clear();
    SHOW_BKG;
    DISPLAY_ON;

    sram_hw_init();                          /* MBC1 RAM banking 模式 */

    screen_clear();
    put_str(2, 3, "GB PASSWORD");
    put_str(6, 5, "VAULT");
    put_str(7, 8, "V0.3");
    put_str(2, 11, "PRESS START");
    put_str(1, 14, "BATTERY CART NEEDED");
    getkey(J_START);

    vault_load();

    while (1) {
        if (!pin_set) {
            setup_pin();
            menu_screen();
        } else {
            if (unlock()) menu_screen();
        }
    }
}
