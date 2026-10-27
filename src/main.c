/*
 * GB Password Vault v0.3.7.1
 * ------------------------------------------------------------------
 * 在真实 Game Boy (DMG / GBC 兼容模式 / GBA) 上运行的离线密码管理器。
 * 编译工具链: GBDK-2020 4.5.0  (C/SDCC)
 * 卡带类型:   MBC1 + 32KB SRAM (4 x 8KB, RAM banking) + BATTERY  (标题 PASSMGR)
 *
 * 功能:
 *   - 首次开机: 设置 6 位数字 PIN (SRAM 中以 +7 取模混淆存储)
 *   - 开机解锁: 3x4 数字键盘输入 PIN, 错误 3 次进入 SYSTEM LOCKED 彩蛋
 *   - 忘记 PIN: 可选 12 位恢复码 (只存双份 hash 到高区扩展块), 输入恢复码可重设 PIN
 *   - 主菜单:   查看账号 / 添加账号 / 修改 PIN / 锁库
 *   - 设置:     生成器长度/符号/自动锁屏/列表视图/创建或移除恢复码
 *   - 提示条:   保存成功等瞬时提示使用屏幕正中的长方形 label
 *   - 账号:     255 个槽位, 每槽 20 字符标签 + 20 字符用户名 + 36 字符密码
 *   - 添加流程: 屏幕虚拟键盘 (大写/小写/符号页三种模式, D-pad 选字 A 输入);
 *              用户名可留空; 密码页按 SELECT 生成 16 位随机强密码
 *   - 列表页:   12 行滚动窗口 + 右上角位置 (nnn/255), 上下步进 / 左右跳 8 条;
 *              选中行标签超过 14 字符时来回跑马灯显示
 *   - 详情页:   按住 A 显示密码明文 (分两行), SELECT+START 编辑, START 删除
 *   - 存档升级: PMG3 验证旧 PIN 后迁移; PMG4 自动就地升级到 PMG5
 *   - 全部数据即时写入 SRAM; PMG5 双头 + 记录 CRC16 + WAL 写前日志
 *
 * SRAM 布局 (平面地址; MBC1 模式 1 下经 0x4000 切 8KB 存储体):
 *   0x0000  PMG4 兼容低头 (MAGIC/PIN/TRIES/XOR/FLAGS/设置)
 *   0x0010  槽位区 255 x 80 字节:
 *          [0] used(0xA5) [1] llen [2..21] label [22] ulen
 *          [23..42] user [43] plen [44..79] pass
 *   0x4FC0  回收站 BIN1 + 16 x 80 字节
 *   0x54D0  PMG5 双头 A
 *   0x54F0  PMG5 双头 B
 *   0x5510  槽位 CRC16 表 255 x 2
 *   0x5710  回收站 CRC16 表 16 x 2
 *   0x5730  恢复码扩展块 A (RCV1, 20B)
 *   0x5750  恢复码扩展块 B (RCV1, 20B)
 *   0x5800  WAL 写前日志 (commit + 变长 old/new 段)
 *   低区继续维护 PMG4 全局 XOR, 便于旧版只读; PMG5 高区是权威格式.
 *
 * v0.3 (PMG3) 旧布局仅用于迁移:
 *   PIN 4 字节 @4, TRIES @8, CHK @9, FLAGS @10, 槽位 255 x 47 字节, 共 12001 字节
 */
#include <gb/gb.h>
#include <stdint.h>
#include <string.h>

#include "font_data.h"

#define N_SLOTS   255
#define LBL_MAX   20
#define USR_MAX   20
#define PSS_MAX   36
#define PIN_LEN    6

/* used+llen+label+ulen+user+plen+pass = 1+1+20+1+20+1+36 = 80 */
#define REC_SZ   (2 + LBL_MAX + 1 + USR_MAX + 1 + PSS_MAX)

#define REC_OFS_USED  0
#define REC_OFS_LLEN  1
#define REC_OFS_LABEL 2
#define REC_OFS_ULEN  (2 + LBL_MAX)                 /* 22 */
#define REC_OFS_USER  (3 + LBL_MAX)                 /* 23 */
#define REC_OFS_PLEN  (3 + LBL_MAX + USR_MAX)       /* 43 */
#define REC_OFS_PASS  (4 + LBL_MAX + USR_MAX)       /* 44 */

#define USED_MAGIC 0xA5
#define FLAG_PINSET 0x01

#define OFS_MAGIC  0
#define OFS_PIN    4
#define OFS_TRIES  10
#define OFS_CHK    11
#define OFS_FLAGS  12
#define OFS_GEN    13
#define OFS_LOCK   14
#define OFS_LIST   15
#define OFS_SLOTS  16

#define SET_VALID  0x80

/* vault_load() 返回值: 损坏/不支持的存档必须停在恢复画面, 不能静默清空 */
#define LOAD_EMPTY        0
#define LOAD_OK           1
#define LOAD_LEGACY       2
#define LOAD_CORRUPT      3
#define LOAD_UNSUPPORTED  4
#define LOAD_PM4          5
#define LOAD_DAMAGED      6

#define SR ((volatile uint8_t *)0xA000)

/* 平面地址: 头部 16 字节 + 255*80 = 20416 字节 (存储体 0/1/2) */
#define REC_ADDR(i)  ((uint16_t)(OFS_SLOTS + (uint16_t)(i) * REC_SZ))
#define VAULT_BYTES  ((uint16_t)(OFS_SLOTS + (uint16_t)N_SLOTS * REC_SZ))

/* 回收站: 槽位区之后的空闲 SRAM, 16 条 × 80B */
#define BIN_SLOTS   16
#define BIN_BASE    VAULT_BYTES
#define BIN_DATA    ((uint16_t)(BIN_BASE + 16))
#define BIN_BYTES   ((uint16_t)(BIN_SLOTS * REC_SZ))

/* v0.3 旧布局 (只用于一次性迁移) */
#define LEGACY_REC_SZ   47
#define LEGACY_BYTES    12001
#define LEGACY_OFS_LLEN 1
#define LEGACY_OFS_LBL  2
#define LEGACY_OFS_PLEN 10
#define LEGACY_OFS_PASS 11

/* 迁移暂存区: 32768 - 255*80 = 12368, 旧档 12001 字节完全不受影响 */
#define MIG_TMP         ((uint16_t)(32768 - N_SLOTS * REC_SZ))

/* MBC1 寄存器: 模式 1 时 0x4000 = RAM 存储体选择, 0x6000 = 模式寄存器 */
#define RAMBANK_REG (*(volatile uint8_t *)0x4000)
#define RAMMODE_REG (*(volatile uint8_t *)0x6000)

typedef struct {
    uint8_t used;
    uint8_t llen;
    char    label[LBL_MAX + 1];
    uint8_t ulen;
    char    user[USR_MAX + 1];
    uint8_t plen;
    char    pass[PSS_MAX + 1];
} slot_t;

static slot_t  cur;             /* 详情页当前槽位缓存 */
static uint8_t pin_obf[PIN_LEN];
static uint8_t pin_set;
static uint8_t tries;

static uint8_t vault_chk;        /* 当前全局 XOR 校验和 (增量维护) */
static uint8_t chk_valid;
static uint8_t pin_idle;         /* 1 = 改 PIN 时也启用自动锁屏 */

/* 用户设置 (存在 PMG4 头 0x0D..0x0F, 旧档全 0 时用默认值) */
static uint8_t gen_idx = 1;      /* 0=12 1=16 2=24 3=32 */
static uint8_t gen_sym = 1;
static uint8_t lock_idx = 2;     /* 0=1 1=2 2=3 3=5 4=10 分钟 */
static uint8_t list_user = 0;    /* 0=标签 1=用户名 */

static uint8_t bin_head;
static uint8_t bin_ready;

static const uint8_t GEN_LENS[4] = { 12, 16, 24, 32 };
static const uint8_t LOCK_MINS[5] = { 1, 2, 3, 5, 10 };

/* ------------------------------------------------------------------ */
/* PIN 画面 2bpp 图形素材                                               */
/*   调色板 BGP=0x9C: 0=最浅 1=黑 2=中绿 3=暗绿                       */
/*   192..200: 9-slice 立体边框 (TL,T,TR,L,F,R,BL,B,BR)                */
/*   201..210: 3x3 PIN 框中心数字 0..9; 211=下划线                     */
/*   212..220: 选中键整体反白的 9-slice 变体                          */
/*   223..237: 键帽底色上的数字/字母 (0-9,C,L,R,O,K)                   */
/*   238..247: 标题行 (SECRET VAULT + 底部横线)                        */
/* ------------------------------------------------------------------ */
static const uint8_t FRAME_TILES[9 * 16] = {
    0x00, 0x00, 0x00, 0x00, 0x3F, 0x00, 0x20, 0x00, 0x20, 0x1F, 0x20, 0x1F, 0x20, 0x1F, 0x20, 0x1F, /* TL */
    0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, /* T  */
    0x00, 0x00, 0x00, 0x00, 0xFC, 0x00, 0x04, 0x00, 0x0C, 0xF8, 0x0C, 0xF8, 0x0C, 0xF8, 0x0C, 0xF8, /* TR */
    0x20, 0x1F, 0x20, 0x1F, 0x20, 0x1F, 0x20, 0x1F, 0x20, 0x1F, 0x20, 0x1F, 0x20, 0x1F, 0x20, 0x1F, /* L  */
    0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, /* F  */
    0x0C, 0xF8, 0x0C, 0xF8, 0x0C, 0xF8, 0x0C, 0xF8, 0x0C, 0xF8, 0x0C, 0xF8, 0x0C, 0xF8, 0x0C, 0xF8, /* R  */
    0x20, 0x1F, 0x20, 0x1F, 0x20, 0x1F, 0x20, 0x1F, 0x3F, 0x1F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, /* BL */
    0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, /* B  */
    0x0C, 0xF8, 0x0C, 0xF8, 0x0C, 0xF8, 0x0C, 0xF8, 0xFC, 0xF8, 0xFC, 0x00, 0x00, 0x00, 0x00, 0x00, /* BR */
};

static const uint8_t TITLE_SEQ[12] = {
    239, 240, 241, 242, 240, 243, 238, 244, 245, 246, 247, 243
};

/* 248..255: 8 个 8x8 单色小图标 (菜单/警告用) */
static const uint8_t ICON_BITS[8][8] = {
    { 0x7E, 0x42, 0x7E, 0x42, 0x7E, 0x42, 0x7E, 0x00 }, /* list  */
    { 0x18, 0x18, 0x7E, 0x7E, 0x18, 0x18, 0x00, 0x00 }, /* plus  */
    { 0x3C, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x30, 0x00 }, /* key   */
    { 0x18, 0x24, 0x42, 0x7E, 0x66, 0x66, 0x7E, 0x00 }, /* lock  */
    { 0x24, 0x7E, 0x42, 0x42, 0x42, 0x42, 0x7E, 0x00 }, /* trash */
    { 0x24, 0x2C, 0x3C, 0x7E, 0x7E, 0x3C, 0x2C, 0x24 }, /* gear  */
    { 0x7E, 0x42, 0x42, 0x24, 0x24, 0x18, 0x18, 0x00 }, /* shield*/
    { 0x18, 0x3C, 0x7E, 0x7E, 0x18, 0x18, 0x18, 0x00 }  /* warn  */
};

/* 生成用源字模 (font_tiles 编号): 0-9, '_' */
static const uint8_t SPLIT_SRC[11] = {
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 63
};
/* 键帽底色字模: 0-9,C,L,R,O,K */
static const uint8_t SRC_LABEL[15] = {
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 35, 44, 50, 47, 43
};
/* 标题字模: 空格,S,E,C,R,T,V,A,U,L */
static const uint8_t SRC_TITLE[10] = {
    0, 51, 37, 35, 50, 52, 54, 33, 53, 44
};

static uint8_t legacy_pin_obf[4];   /* v0.3 旧档头部 (迁移用) */
static uint8_t legacy_tries;

static const uint8_t MAGIC[4]  = { 'P', 'M', 'G', '4' };
static const uint8_t MAGIC3[4] = { 'P', 'M', 'G', '3' };

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

/* ------------------------------------------------------------------ */
/* PMG5: 兼容 PMG4 数据区 + 高区双头 / 记录 CRC16 / 写前日志             */
/*                                                                      */
/* 低区 0x0000..0x54CF 仍保持 PMG4 布局与全局 XOR 校验和, 以便旧版只读   */
/* 兼容; PMG5 的权威头在 0x54D0/0x54F0, 记录 CRC 表在 0x5510, WAL 在     */
/* 0x5800. 所有正常写操作先写 WAL, 再改目标, 最后清 WAL.                */
/* ------------------------------------------------------------------ */

#define PM5_HDR_A       ((uint16_t)0x54D0)
#define PM5_HDR_B       ((uint16_t)0x54F0)
#define PM5_HDR_SIZE    32
#define PM5_H_VER       4
#define PM5_H_STATE     5
#define PM5_H_SEQ       6
#define PM5_H_CRC       8
#define PM5_H_PIN       10
#define PM5_H_TRIES     16
#define PM5_H_FLAGS     17
#define PM5_H_GEN       18
#define PM5_H_LOCK      19
#define PM5_H_LIST      20
#define PM5_VERSION     1
#define PM5_READY       1

#define PM5_REC_CRC     ((uint16_t)0x5510)   /* 255 * 2 = 510 */
#define PM5_BIN_CRC     ((uint16_t)0x5710)   /* 16 * 2 = 32  */
#define PM5_RCV_A       ((uint16_t)0x5730)   /* 恢复码 hash 块 A */
#define PM5_RCV_B       ((uint16_t)0x5750)   /* 恢复码 hash 块 B */
#define PM5_RCV_SIZE    20
#define RCV_VERSION     1
#define RCV_FLAG_SET    0x01
#define RCV_OFS_MAGIC   0
#define RCV_OFS_VER     4
#define RCV_OFS_FLAGS   5
#define RCV_OFS_SEQ     6
#define RCV_OFS_HASH    8
#define RCV_HASH_LEN    8
#define RCV_OFS_CRC     16
#define RCV_CODE_LEN    12
#define PM5_WAL_BASE    ((uint16_t)0x5800)
#define PM5_WAL_MAX     256
#define PM5_WAL_COMMIT  PM5_WAL_BASE
#define PM5_WAL_HDR     10
#define PM5_WAL_PAYLOAD (PM5_WAL_BASE + PM5_WAL_HDR)

#define FLAG_PM5        0x80

static uint8_t  pm5_active;
static uint16_t pm5_seq;
static uint8_t  bad_slots[(N_SLOTS + 7) / 8];
static uint8_t  bad_bin[(BIN_SLOTS + 7) / 8];
static uint16_t vault_bad_count;
static uint8_t  damage_ack;
static uint8_t  rcv_set;
static uint16_t rcv_seq;
static uint8_t  rcv_hash[RCV_HASH_LEN];
static void pm5_build_tables(void);
static void pm5_scan_crc(void);
static uint16_t bin_crc_addr(uint8_t i);
static uint8_t  wal_buf[PM5_WAL_MAX];
static uint16_t wal_len;
static uint8_t  wal_nseg;

/* CRC16/CCITT-FALSE: init 0xFFFF, poly 0x1021, 不反射、不 xorout. */
static uint16_t crc16_update(uint16_t c, uint8_t b)
{
    uint8_t i;
    c ^= (uint16_t)((uint16_t)b << 8);
    for (i = 0; i < 8; i++)
        c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    return c;
}

static uint16_t crc16_buf(const uint8_t *p, uint16_t n)
{
    uint16_t c = 0xFFFF;
    while (n--) c = crc16_update(c, *p++);
    return c;
}

/* 调用者必须已 sram_on(). */
static uint16_t crc16_sram(uint16_t a, uint16_t n)
{
    uint16_t c = 0xFFFF;
    while (n--) c = crc16_update(c, rb(a++));
    return c;
}

/* 调用者必须已 sram_on(). */
static uint16_t rd16(uint16_t a)
{
    return (uint16_t)((uint16_t)rb(a) | ((uint16_t)rb((uint16_t)(a + 1)) << 8));
}

/* 调用者必须已 sram_on(). */
static void wr16(uint16_t a, uint16_t v)
{
    wb(a, (uint8_t)(v & 0xFF));
    wb((uint16_t)(a + 1), (uint8_t)(v >> 8));
}

static void pm5_hdr_write(uint16_t a, uint16_t seq, uint8_t state)
{
    uint8_t b[PM5_HDR_SIZE];
    uint8_t i;
    uint16_t c = 0xFFFF;

    memset(b, 0, sizeof(b));
    b[0] = 'P'; b[1] = 'M'; b[2] = 'G'; b[3] = '5';
    b[PM5_H_VER] = PM5_VERSION;
    b[PM5_H_STATE] = state;
    b[PM5_H_SEQ] = (uint8_t)(seq & 0xFF);
    b[PM5_H_SEQ + 1] = (uint8_t)(seq >> 8);
    for (i = 0; i < PIN_LEN; i++) b[PM5_H_PIN + i] = pin_obf[i];
    b[PM5_H_TRIES] = tries;
    b[PM5_H_FLAGS] = pin_set ? FLAG_PINSET : 0x00;
    b[PM5_H_GEN] = (uint8_t)(gen_idx | (gen_sym ? 0x04 : 0x00) | SET_VALID);
    b[PM5_H_LOCK] = lock_idx;
    b[PM5_H_LIST] = list_user ? 1 : 0;
    for (i = 0; i < PM5_HDR_SIZE; i++) {
        if (i != PM5_H_CRC && i != PM5_H_CRC + 1)
            c = crc16_update(c, b[i]);
    }
    b[PM5_H_CRC] = (uint8_t)(c & 0xFF);
    b[PM5_H_CRC + 1] = (uint8_t)(c >> 8);

    sram_on();
    for (i = 0; i < PM5_HDR_SIZE; i++) wb((uint16_t)(a + i), b[i]);
    sram_off();
}

static uint8_t pm5_hdr_valid(uint16_t a, uint16_t *seq)
{
    uint8_t i;
    uint16_t c = 0xFFFF;

    if (rb(a) != 'P' || rb((uint16_t)(a + 1)) != 'M' ||
        rb((uint16_t)(a + 2)) != 'G' || rb((uint16_t)(a + 3)) != '5') return 0;
    if (rb((uint16_t)(a + PM5_H_VER)) != PM5_VERSION) return 0;
    if (rb((uint16_t)(a + PM5_H_STATE)) != PM5_READY) return 0;
    for (i = 0; i < PM5_HDR_SIZE; i++) {
        if (i != PM5_H_CRC && i != PM5_H_CRC + 1)
            c = crc16_update(c, rb((uint16_t)(a + i)));
    }
    if (c != rd16((uint16_t)(a + PM5_H_CRC))) return 0;
    *seq = rd16((uint16_t)(a + PM5_H_SEQ));
    return 1;
}

/* 读取双头中有效且 seq 较新的一个; 成功时加载 PIN/设置并设置 pm5_active. */
static uint8_t pm5_hdr_load(void)
{
    uint16_t sa = 0, sb = 0, a, s;
    uint8_t va, vb, i, g;

    sram_on();
    va = pm5_hdr_valid(PM5_HDR_A, &sa);
    vb = pm5_hdr_valid(PM5_HDR_B, &sb);
    if (!va && !vb) { sram_off(); return 0; }
    if (va && (!vb || sa >= sb)) { a = PM5_HDR_A; s = sa; }
    else                         { a = PM5_HDR_B; s = sb; }

    for (i = 0; i < PIN_LEN; i++) pin_obf[i] = rb((uint16_t)(a + PM5_H_PIN + i));
    tries = rb((uint16_t)(a + PM5_H_TRIES));
    if (tries > 3) tries = 3;
    pin_set = (rb((uint16_t)(a + PM5_H_FLAGS)) & FLAG_PINSET) ? 1 : 0;
    g = rb((uint16_t)(a + PM5_H_GEN));
    if (g & SET_VALID) {
        gen_idx = (uint8_t)(g & 3);
        if (gen_idx > 3) gen_idx = 1;
        gen_sym = (g & 0x04) ? 1 : 0;
        lock_idx = rb((uint16_t)(a + PM5_H_LOCK));
        if (lock_idx > 4) lock_idx = 2;
        list_user = (uint8_t)(rb((uint16_t)(a + PM5_H_LIST)) & 1);
    } else {
        gen_idx = 1; gen_sym = 1; lock_idx = 2; list_user = 0;
    }
    pm5_seq = s;
    pm5_active = 1;
    bin_ready = 0;
    sram_off();
    return 1;
}

static void pm5_hdr_save(void)
{
    uint16_t s = (uint16_t)(pm5_seq + 1);
    pm5_seq = s;
    pm5_hdr_write(PM5_HDR_A, s, PM5_READY);
    pm5_hdr_write(PM5_HDR_B, s, PM5_READY);
}

/* ------------------------------------------------------------------ */
/* WAL: 固定提交字节 + 变长 payload; payload 为若干 old/new 段.           */
/* 段格式: addr_lo, addr_hi, len, old[len], new[len]                     */
/* ------------------------------------------------------------------ */

static void wal_reset(void)
{
    wal_len = 0;
    wal_nseg = 0;
}

static void wal_add(uint16_t a, uint8_t n, const uint8_t *oldb, const uint8_t *newb)
{
    uint8_t i;
    if ((uint16_t)(wal_len + 4u + (uint16_t)n * 2u) > PM5_WAL_MAX) return;
    wal_buf[wal_len++] = (uint8_t)(a & 0xFF);
    wal_buf[wal_len++] = (uint8_t)(a >> 8);
    wal_buf[wal_len++] = n;
    for (i = 0; i < n; i++) wal_buf[wal_len++] = oldb[i];
    for (i = 0; i < n; i++) wal_buf[wal_len++] = newb[i];
    wal_nseg++;
}

static void wal_clear(void)
{
    sram_on();
    wb(PM5_WAL_COMMIT, 0x00);
    sram_off();
}

static void wal_commit(void)
{
    uint8_t i;
    uint16_t c = 0xFFFF;

    sram_on();
    wb(PM5_WAL_COMMIT, 0x00);       /* 先让旧日志失效 */
    wb((uint16_t)(PM5_WAL_BASE + 1), 'W');
    wb((uint16_t)(PM5_WAL_BASE + 2), 'A');
    wb((uint16_t)(PM5_WAL_BASE + 3), 'L');
    wb((uint16_t)(PM5_WAL_BASE + 4), '5');
    wb((uint16_t)(PM5_WAL_BASE + 5), wal_nseg);
    wb((uint16_t)(PM5_WAL_BASE + 6), (uint8_t)(wal_len & 0xFF));
    wb((uint16_t)(PM5_WAL_BASE + 7), (uint8_t)(wal_len >> 8));
    /* CRC 覆盖 base+1..+7 和 payload, 跳过 +8/+9 自身 */
    for (i = 1; i <= 7; i++) c = crc16_update(c, rb((uint16_t)(PM5_WAL_BASE + i)));
    for (i = 0; i < wal_len; i++) c = crc16_update(c, wal_buf[i]);
    wb((uint16_t)(PM5_WAL_BASE + 8), (uint8_t)(c & 0xFF));
    wb((uint16_t)(PM5_WAL_BASE + 9), (uint8_t)(c >> 8));
    for (i = 0; i < wal_len; i++) wb((uint16_t)(PM5_WAL_PAYLOAD + i), wal_buf[i]);
    wb(PM5_WAL_COMMIT, 0xA5);       /* 最后提交 */
    sram_off();
}

/* 上电恢复: 已提交日志重放 new 段; 未提交/损坏日志直接丢弃. */
static void wal_recover(void)
{
    uint8_t i, nseg;
    uint16_t len, c = 0xFFFF;
    uint16_t p;

    sram_on();
    if (rb(PM5_WAL_COMMIT) != 0xA5) { sram_off(); return; }
    /* 只有 PMG5 双头存在时才重放; 防止把旧 PMG4 存档恢复后残留的 WAL 应用上去. */
    {
        uint16_t sa, sb;
        if (!pm5_hdr_valid(PM5_HDR_A, &sa) && !pm5_hdr_valid(PM5_HDR_B, &sb)) {
            wb(PM5_WAL_COMMIT, 0x00);
            sram_off();
            return;
        }
    }
    if (rb((uint16_t)(PM5_WAL_BASE + 1)) != 'W' ||
        rb((uint16_t)(PM5_WAL_BASE + 2)) != 'A' ||
        rb((uint16_t)(PM5_WAL_BASE + 3)) != 'L' ||
        rb((uint16_t)(PM5_WAL_BASE + 4)) != '5') {
        wb(PM5_WAL_COMMIT, 0x00); sram_off(); return;
    }
    nseg = rb((uint16_t)(PM5_WAL_BASE + 5));
    len = (uint16_t)((uint16_t)rb((uint16_t)(PM5_WAL_BASE + 6)) |
                     ((uint16_t)rb((uint16_t)(PM5_WAL_BASE + 7)) << 8));
    if (len > PM5_WAL_MAX || nseg == 0 || nseg > 8) {
        wb(PM5_WAL_COMMIT, 0x00); sram_off(); return;
    }
    for (i = 1; i <= 7; i++) c = crc16_update(c, rb((uint16_t)(PM5_WAL_BASE + i)));
    for (i = 0; i < len; i++) {
        wal_buf[i] = rb((uint16_t)(PM5_WAL_PAYLOAD + i));
        c = crc16_update(c, wal_buf[i]);
    }
    if (c != rd16((uint16_t)(PM5_WAL_BASE + 8))) {
        wb(PM5_WAL_COMMIT, 0x00); sram_off(); return;
    }

    p = 0;
    for (i = 0; i < nseg; i++) {
        uint16_t a;
        uint8_t n, j;
        if ((uint16_t)(p + 3) > len) break;
        a = (uint16_t)((uint16_t)wal_buf[p] | ((uint16_t)wal_buf[p + 1] << 8));
        n = wal_buf[p + 2];
        p = (uint16_t)(p + 3);
        if ((uint16_t)(p + (uint16_t)n * 2u) > len) break;
        p = (uint16_t)(p + n);       /* 跳过 old */
        for (j = 0; j < n; j++) wb((uint16_t)(a + j), wal_buf[p + j]);
        p = (uint16_t)(p + n);
    }
    wb(PM5_WAL_COMMIT, 0x00);       /* 幂等重放完成 */
    sram_off();
}

/* ------------------------------------------------------------------ */
/* 恢复码扩展块: 可选 12 位恢复码, 只保存双份 64-bit hash.              */
/* 块格式 (20B): "RCV1"(4) ver(1) flags(1) seq(2) hash(8) crc16(2)     */
/* reserved(2). 双块 + seq + CRC, 写入走 WAL, 断电不会留下半块.        */
/* ------------------------------------------------------------------ */

static void rcv_cache_clear(void)
{
    rcv_set = 0;
    rcv_seq = 0;
    memset(rcv_hash, 0, sizeof(rcv_hash));
}

/* 两个 32 位 FNV-1a 变体拼接; 只用于恢复码校验, 不是通用加密哈希. */
static void recovery_hash(const uint8_t *code, uint8_t *out)
{
    uint32_t h1 = 2166136261UL;
    uint32_t h2 = 2166136261UL ^ 0x9E3779B9UL;
    uint8_t i, d;

    for (i = 0; i < RCV_CODE_LEN; i++) {
        d = (uint8_t)(code[i] + 1u);
        h1 ^= (uint32_t)d;
        h1 *= 16777619UL;
        h2 ^= (uint32_t)(uint8_t)(d ^ 0x5Au);
        h2 *= 16777619UL;
    }
    h1 ^= 0x504D4735UL;                 /* "PMG5" */
    h2 ^= 0x52435631UL;                 /* "RCV1" */
    out[0] = (uint8_t)h1; out[1] = (uint8_t)(h1 >> 8);
    out[2] = (uint8_t)(h1 >> 16); out[3] = (uint8_t)(h1 >> 24);
    out[4] = (uint8_t)h2; out[5] = (uint8_t)(h2 >> 8);
    out[6] = (uint8_t)(h2 >> 16); out[7] = (uint8_t)(h2 >> 24);
}

/* 调用者必须已 sram_on(). */
static uint8_t pm5_rcv_valid(uint16_t a, uint16_t *seq, uint8_t *present,
                             uint8_t *hash)
{
    uint8_t i;
    uint16_t c = 0xFFFF;

    if (rb(a) != 'R' || rb((uint16_t)(a + 1)) != 'C' ||
        rb((uint16_t)(a + 2)) != 'V' || rb((uint16_t)(a + 3)) != '1') return 0;
    if (rb((uint16_t)(a + RCV_OFS_VER)) != RCV_VERSION) return 0;
    for (i = 0; i < RCV_OFS_CRC; i++)
        c = crc16_update(c, rb((uint16_t)(a + i)));
    if (c != rd16((uint16_t)(a + RCV_OFS_CRC))) return 0;
    *seq = rd16((uint16_t)(a + RCV_OFS_SEQ));
    *present = (rb((uint16_t)(a + RCV_OFS_FLAGS)) & RCV_FLAG_SET) ? 1 : 0;
    for (i = 0; i < RCV_HASH_LEN; i++)
        hash[i] = rb((uint16_t)(a + RCV_OFS_HASH + i));
    return 1;
}

/* 读取双块中有效且 seq 较新的一个; 无有效块时清除缓存. */
static void pm5_rcv_load(void)
{
    uint8_t va, vb, pa = 0, pb = 0, ha[RCV_HASH_LEN], hb[RCV_HASH_LEN];
    uint16_t sa = 0, sb = 0;

    sram_on();
    va = pm5_rcv_valid(PM5_RCV_A, &sa, &pa, ha);
    vb = pm5_rcv_valid(PM5_RCV_B, &sb, &pb, hb);
    sram_off();
    if (!va && !vb) { rcv_cache_clear(); return; }
    if (va && (!vb || sa >= sb)) {
        rcv_seq = sa;
        rcv_set = pa;
        memcpy(rcv_hash, ha, RCV_HASH_LEN);
    } else {
        rcv_seq = sb;
        rcv_set = pb;
        memcpy(rcv_hash, hb, RCV_HASH_LEN);
    }
}

static void rcv_block_build(uint8_t *b, uint16_t seq, uint8_t present,
                            const uint8_t *hash)
{
    uint8_t i;
    uint16_t c = 0xFFFF;

    memset(b, 0, PM5_RCV_SIZE);
    b[0] = 'R'; b[1] = 'C'; b[2] = 'V'; b[3] = '1';
    b[RCV_OFS_VER] = RCV_VERSION;
    b[RCV_OFS_FLAGS] = present ? RCV_FLAG_SET : 0x00;
    b[RCV_OFS_SEQ] = (uint8_t)(seq & 0xFF);
    b[RCV_OFS_SEQ + 1] = (uint8_t)(seq >> 8);
    for (i = 0; i < RCV_HASH_LEN; i++) b[RCV_OFS_HASH + i] = hash[i];
    for (i = 0; i < RCV_OFS_CRC; i++) c = crc16_update(c, b[i]);
    b[RCV_OFS_CRC] = (uint8_t)(c & 0xFF);
    b[RCV_OFS_CRC + 1] = (uint8_t)(c >> 8);
}

/* 原子写双块: WAL 记录旧/新块, 提交后写目标, 最后清 commit. */
static void pm5_rcv_write_pair(uint16_t seq, uint8_t present,
                               const uint8_t *hash)
{
    uint8_t olda[PM5_RCV_SIZE], oldb[PM5_RCV_SIZE];
    uint8_t newa[PM5_RCV_SIZE], newb[PM5_RCV_SIZE];
    uint8_t i;

    sram_on();
    for (i = 0; i < PM5_RCV_SIZE; i++) {
        olda[i] = rb((uint16_t)(PM5_RCV_A + i));
        oldb[i] = rb((uint16_t)(PM5_RCV_B + i));
    }
    sram_off();
    rcv_block_build(newa, seq, present, hash);
    rcv_block_build(newb, seq, present, hash);

    wal_reset();
    wal_add(PM5_RCV_A, PM5_RCV_SIZE, olda, newa);
    wal_add(PM5_RCV_B, PM5_RCV_SIZE, oldb, newb);
    wal_commit();

    sram_on();
    for (i = 0; i < PM5_RCV_SIZE; i++) {
        wb((uint16_t)(PM5_RCV_A + i), newa[i]);
        wb((uint16_t)(PM5_RCV_B + i), newb[i]);
    }
    sram_off();
    wal_clear();
}

static void pm5_rcv_save(const uint8_t *hash)
{
    rcv_seq = (uint16_t)(rcv_seq + 1);
    pm5_rcv_write_pair(rcv_seq, 1, hash);
    rcv_set = 1;
    memcpy(rcv_hash, hash, RCV_HASH_LEN);
}

static void pm5_rcv_clear(void)
{
    uint8_t zero[RCV_HASH_LEN];

    memset(zero, 0, sizeof(zero));
    rcv_seq = (uint16_t)(rcv_seq + 1);
    pm5_rcv_write_pair(rcv_seq, 0, zero);
    rcv_set = 0;
    memset(rcv_hash, 0, sizeof(rcv_hash));
}

static uint8_t pm5_rcv_verify(const uint8_t *code)
{
    uint8_t h[RCV_HASH_LEN];
    uint8_t i;

    recovery_hash(code, h);
    for (i = 0; i < RCV_HASH_LEN; i++)
        if (h[i] != rcv_hash[i]) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* 低区 PMG4 头 / 兼容层                                                */
/* ------------------------------------------------------------------ */

static uint8_t chk_calc(void)
{
    uint8_t c = 0xA5;
    uint8_t b;
    uint16_t i, a = 0;

    for (b = 0; b < 3; b++) {
        bank_set(b);
        for (i = 0; i < 8192; i++, a++) {
            if (a >= VAULT_BYTES) return c;
            if (a != OFS_CHK) c ^= SR[i];
        }
    }
    return c;
}

/* v0.3 旧档校验和: 0..12000, 跳过旧 CHK 字节 9 */
static uint8_t legacy_chk_calc(void)
{
    uint8_t c = 0xA5;
    uint16_t i;
    for (i = 0; i < 16; i++) {
        if (i != 9) c ^= rb(i);
    }
    for (i = 16; i < LEGACY_BYTES; i++) {
        c ^= rb(i);
    }
    return c;
}

static uint8_t magic_is(const uint8_t *m)
{
    uint8_t i;
    for (i = 0; i < 4; i++) {
        if (rb(OFS_MAGIC + i) != m[i]) return 0;
    }
    return 1;
}

/* 写低区 PMG4 兼容头 + 高区 PMG5 双头. 低区校验和保持有效, 方便旧版只读. */
static void header_save(void)
{
    uint8_t i;
    sram_on();
    for (i = 0; i < 4; i++) wb(OFS_MAGIC + i, MAGIC[i]);
    for (i = 0; i < PIN_LEN; i++) wb(OFS_PIN + i, pin_obf[i]);
    wb(OFS_TRIES, tries);
    wb(OFS_FLAGS, (uint8_t)((pin_set ? FLAG_PINSET : 0x00) | (pm5_active ? FLAG_PM5 : 0x00)));
    wb(OFS_GEN, (uint8_t)(gen_idx | (gen_sym ? 0x04 : 0x00) | SET_VALID));
    wb(OFS_LOCK, lock_idx);
    wb(OFS_LIST, list_user ? 1 : 0);
    vault_chk = chk_calc();
    wb(OFS_CHK, vault_chk);
    chk_valid = 1;
    sram_off();
    if (pm5_active) pm5_hdr_save();
}

/* 记录改动提交: 低区 PMG4 兼容校验和已在写入时增量维护. */
static void vault_commit(void)
{
    sram_on();
    wb(OFS_CHK, vault_chk);
    sram_off();
}

static void settings_save(void)
{
    header_save();
}

/* 读取低区 PMG4 字段; 调用者已 sram_on. */
static void pmg4_fields_load(void)
{
    uint8_t i, g;
    for (i = 0; i < PIN_LEN; i++) pin_obf[i] = rb(OFS_PIN + i);
    tries = rb(OFS_TRIES);
    if (tries > 3) tries = 3;
    pin_set = (rb(OFS_FLAGS) & FLAG_PINSET) ? 1 : 0;
    g = rb(OFS_GEN);
    if (g & SET_VALID) {
        gen_idx = (uint8_t)(g & 3);
        if (gen_idx > 3) gen_idx = 1;
        gen_sym = (g & 0x04) ? 1 : 0;
        lock_idx = rb(OFS_LOCK);
        if (lock_idx > 4) lock_idx = 2;
        list_user = (uint8_t)(rb(OFS_LIST) & 1);
    } else {
        gen_idx = 1; gen_sym = 1; lock_idx = 2; list_user = 0;
    }
}

/* 低区 PMG4 头是否与高区 PMG5 头一致; 调用者已 sram_on. */
static uint8_t pm5_low_matches(void)
{
    uint8_t i;
    if (!magic_is(MAGIC)) return 0;
    if (rb(OFS_CHK) != chk_calc()) return 0;
    for (i = 0; i < PIN_LEN; i++)
        if (rb((uint16_t)(OFS_PIN + i)) != pin_obf[i]) return 0;
    if (rb(OFS_TRIES) != tries) return 0;
    if ((rb(OFS_FLAGS) & FLAG_PINSET) != (pin_set ? FLAG_PINSET : 0)) return 0;
    if (rb(OFS_GEN) != (uint8_t)(gen_idx | (gen_sym ? 0x04 : 0x00) | SET_VALID)) return 0;
    if (rb(OFS_LOCK) != lock_idx) return 0;
    if (rb(OFS_LIST) != (uint8_t)(list_user ? 1 : 0)) return 0;
    return 1;
}

/* 只在启动/迁移后扫描一次, 避免列表每次绘制记录都算 CRC. */
static void pm5_scan_crc(void)
{
    uint16_t i;
    uint16_t c;
    memset(bad_slots, 0, sizeof(bad_slots));
    memset(bad_bin, 0, sizeof(bad_bin));
    vault_bad_count = 0;
    if (!pm5_active) return;
    sram_on();
    for (i = 0; i < N_SLOTS; i++) {
        if (rb(REC_ADDR(i)) != USED_MAGIC) continue;
        c = crc16_sram(REC_ADDR(i), REC_SZ);
        if (c != rd16((uint16_t)(PM5_REC_CRC + i * 2u))) {
            bad_slots[i >> 3] |= (uint8_t)(1u << (i & 7));
            vault_bad_count++;
        }
    }
    for (i = 0; i < BIN_SLOTS; i++) {
        if (rb((uint16_t)(BIN_DATA + i * REC_SZ)) != USED_MAGIC) continue;
        c = crc16_sram((uint16_t)(BIN_DATA + i * REC_SZ), REC_SZ);
        if (c != rd16(bin_crc_addr((uint8_t)i))) {
            bad_bin[i >> 3] |= (uint8_t)(1u << (i & 7));
            vault_bad_count++;
        }
    }
    sram_off();
}

/* 读入头部.
 * 返回 LOAD_EMPTY / LOAD_OK / LOAD_LEGACY / LOAD_PM4 / LOAD_CORRUPT / LOAD_UNSUPPORTED.
 * PMG5 双头有效时优先使用高区; 否则低区 PMG4 有效时返回 LOAD_PM4 走自动迁移.
 * 有 MAGIC 但校验失败绝不能当空档. */
static uint8_t vault_load(void)
{
    uint8_t i, unknown = 0;

    if (pm5_hdr_load()) {
        pm5_rcv_load();
        sram_on();
        if (magic_is(MAGIC) && rb(OFS_CHK) == chk_calc()) {
            if (pm5_low_matches()) {
                vault_chk = rb(OFS_CHK);
                chk_valid = 1;
                sram_off();
                pm5_scan_crc();
                return (uint8_t)((vault_bad_count && !damage_ack) ? LOAD_DAMAGED : LOAD_OK);
            }
            /* 低区被旧版/外部修改: 以低区为准, 重新迁移并重建高区. */
            pmg4_fields_load();
            pm5_active = 0;
            vault_chk = rb(OFS_CHK);
            chk_valid = 1;
            sram_off();
            return LOAD_PM4;
        }
        chk_valid = 0;
        sram_off();
        pm5_scan_crc();
        return (uint8_t)((vault_bad_count && !damage_ack) ? LOAD_DAMAGED : LOAD_OK);
    }

    sram_on();
    if (magic_is(MAGIC)) {
        if (rb(OFS_CHK) == chk_calc()) {
            pm5_active = 0;
            pmg4_fields_load();
            vault_chk = rb(OFS_CHK);
            chk_valid = 1;
            sram_off();
            pm5_rcv_load();
            return LOAD_PM4;
        }
        sram_off();
        rcv_cache_clear();
        return LOAD_CORRUPT;
    }

    if (magic_is(MAGIC3)) {
        if (rb(9) == legacy_chk_calc()) {
            for (i = 0; i < 4; i++) legacy_pin_obf[i] = rb(OFS_PIN + i);
            legacy_tries = rb(8);
            if (legacy_tries > 3) legacy_tries = 3;
            sram_off();
            rcv_cache_clear();           /* v0.3 没有恢复码, 迁移会清高区 */
            return LOAD_LEGACY;
        }
        sram_off();
        rcv_cache_clear();
        return LOAD_CORRUPT;
    }

    /* 空白卡带: 前 4 字节全为 0x00/0xFF. 其他未知数据不能静默重建. */
    for (i = 0; i < 4; i++) {
        uint8_t b = rb(OFS_MAGIC + i);
        if (b != 0x00 && b != 0xFF) unknown = 1;
    }
    sram_off();

    pm5_active = 0;
    pin_set = 0;
    tries = 0;
    chk_valid = 0;
    gen_idx = 1; gen_sym = 1; lock_idx = 2; list_user = 0;
    for (i = 0; i < PIN_LEN; i++) pin_obf[i] = 0;
    rcv_cache_clear();
    return unknown ? LOAD_UNSUPPORTED : LOAD_EMPTY;
}

/* 槽位区整体清零 (清库/首次设置; 不碰低区头部; 同时清回收站/日志/高区) */
static void vault_clear_slots(void)
{
    sram_on();
    bank_set(0);
    memset((void *)(SR + OFS_SLOTS), 0, (uint16_t)(8192 - OFS_SLOTS));
    bank_set(1);
    memset((void *)SR, 0, 8192);
    bank_set(2);
    memset((void *)SR, 0, 8192);
    bank_set(3);
    memset((void *)SR, 0, 8192);
    sram_off();
    chk_valid = 0;
    bin_ready = 0;
    bin_head = 0;
    rcv_cache_clear();
}

/* SYSTEM LOCKED 彩蛋: 擦除全部数据 */
static void vault_wipe(void)
{
    uint8_t i;
    for (i = 0; i < PIN_LEN; i++) pin_obf[i] = 0;
    pin_set = 0;
    tries = 0;
    gen_idx = 1; gen_sym = 1; lock_idx = 2; list_user = 0;
    pm5_active = 1;
    damage_ack = 0;
    vault_clear_slots();
    pm5_build_tables();
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
    if (u && pm5_active && (bad_slots[i >> 3] & (uint8_t)(1u << (i & 7)))) u = 0;
    return u;
}

static uint8_t slot_raw_used(uint16_t i)
{
    uint8_t u;
    sram_on();
    u = (uint8_t)(rb(REC_ADDR(i)) == USED_MAGIC);
    sram_off();
    return u;
}

/* 在当前布局的任意平面地址读/写一条记录 (迁移暂存区也用它) */
static void rec_read_raw(uint16_t a, uint8_t *b)
{
    uint8_t j;
    sram_on();
    for (j = 0; j < REC_SZ; j++) b[j] = rb((uint16_t)(a + j));
    sram_off();
}

static void rec_write_raw(uint16_t a, const uint8_t *b)
{
    uint8_t j;
    sram_on();
    for (j = 0; j < REC_SZ; j++) wb((uint16_t)(a + j), b[j]);
    sram_off();
}

static void rec_pack(const slot_t *s, uint8_t *b)
{
    uint8_t j;
    memset(b, 0, REC_SZ);
    b[REC_OFS_USED] = s->used ? USED_MAGIC : 0x00;
    b[REC_OFS_LLEN] = s->llen;
    for (j = 0; j < LBL_MAX; j++) b[REC_OFS_LABEL + j] = (uint8_t)s->label[j];
    b[REC_OFS_ULEN] = s->ulen;
    for (j = 0; j < USR_MAX; j++) b[REC_OFS_USER + j] = (uint8_t)s->user[j];
    b[REC_OFS_PLEN] = s->plen;
    for (j = 0; j < PSS_MAX; j++) b[REC_OFS_PASS + j] = (uint8_t)s->pass[j];
}

static void rec_read_at(uint16_t a, slot_t *s)
{
    uint8_t j;
    uint8_t b[REC_SZ];

    memset(s, 0, sizeof(slot_t));
    rec_read_raw(a, b);
    if (b[REC_OFS_USED] != USED_MAGIC) return;
    s->used = 1;
    s->llen = b[REC_OFS_LLEN];
    if (s->llen > LBL_MAX) s->llen = LBL_MAX;
    for (j = 0; j < LBL_MAX; j++) s->label[j] = (char)b[REC_OFS_LABEL + j];
    s->label[LBL_MAX] = 0;
    s->ulen = b[REC_OFS_ULEN];
    if (s->ulen > USR_MAX) s->ulen = USR_MAX;
    for (j = 0; j < USR_MAX; j++) s->user[j] = (char)b[REC_OFS_USER + j];
    s->user[USR_MAX] = 0;
    s->plen = b[REC_OFS_PLEN];
    if (s->plen > PSS_MAX) s->plen = PSS_MAX;
    for (j = 0; j < PSS_MAX; j++) s->pass[j] = (char)b[REC_OFS_PASS + j];
    s->pass[PSS_MAX] = 0;
}

static void rec_write_at(uint16_t a, const slot_t *s)
{
    uint8_t b[REC_SZ];
    rec_pack(s, b);
    rec_write_raw(a, b);
}

static void slot_read(uint16_t i, slot_t *s)
{
    rec_read_at(REC_ADDR(i), s);
    if (s->used && pm5_active && (bad_slots[i >> 3] & (uint8_t)(1u << (i & 7))))
        memset(s, 0, sizeof(slot_t));
}

/* 写一条正式槽位记录: WAL 保护 记录 + CRC 表项 + 低区 XOR 校验和. */
static void slot_write(uint16_t i, const slot_t *s)
{
    uint16_t a = REC_ADDR(i);
    uint16_t ca = (uint16_t)(PM5_REC_CRC + (uint16_t)i * 2u);
    uint8_t oldrec[REC_SZ], newrec[REC_SZ];
    uint8_t oldc[2], newc[2], oldchk, newchk, j, x = 0;
    uint16_t nc;

    rec_read_raw(a, oldrec);
    rec_pack(s, newrec);
    sram_on();
    oldc[0] = rb(ca); oldc[1] = rb((uint16_t)(ca + 1));
    oldchk = chk_valid ? vault_chk : 0;
    sram_off();

    for (j = 0; j < REC_SZ; j++) x ^= (uint8_t)(oldrec[j] ^ newrec[j]);
    if (chk_valid) newchk = (uint8_t)(oldchk ^ x);
    else           newchk = oldchk;

    nc = crc16_buf(newrec, REC_SZ);
    newc[0] = (uint8_t)(nc & 0xFF); newc[1] = (uint8_t)(nc >> 8);

    wal_reset();
    wal_add(a, REC_SZ, oldrec, newrec);
    wal_add(ca, 2, oldc, newc);
    if (chk_valid) wal_add(OFS_CHK, 1, &oldchk, &newchk);
    wal_commit();

    rec_write_raw(a, newrec);
    sram_on();
    wb(ca, newc[0]); wb((uint16_t)(ca + 1), newc[1]);
    if (chk_valid) wb(OFS_CHK, newchk);
    sram_off();
    vault_chk = newchk;
    if (!chk_valid) {
        sram_on(); vault_chk = chk_calc(); wb(OFS_CHK, vault_chk); sram_off();
        chk_valid = 1;
    }
    if (pm5_active && (bad_slots[i >> 3] & (uint8_t)(1u << (i & 7)))) {
        bad_slots[i >> 3] &= (uint8_t)~(1u << (i & 7));
        if (vault_bad_count) vault_bad_count--;
    }
    wal_clear();
}

/* v0.3 旧记录 (标签 8 / 密码 36, 无用户名) */
static void legacy_slot_read(uint16_t i, slot_t *s)
{
    uint16_t a = (uint16_t)(OFS_SLOTS + i * LEGACY_REC_SZ);
    uint8_t j;

    memset(s, 0, sizeof(slot_t));
    sram_on();
    if (rb(a) != USED_MAGIC) { sram_off(); return; }
    s->used = 1;
    s->llen = rb((uint16_t)(a + LEGACY_OFS_LLEN));
    if (s->llen > 8) s->llen = 8;
    for (j = 0; j < 8; j++) s->label[j] = (char)rb((uint16_t)(a + LEGACY_OFS_LBL + j));
    s->label[8] = 0;
    s->plen = rb((uint16_t)(a + LEGACY_OFS_PLEN));
    if (s->plen > PSS_MAX) s->plen = PSS_MAX;
    for (j = 0; j < PSS_MAX; j++) s->pass[j] = (char)rb((uint16_t)(a + LEGACY_OFS_PASS + j));
    s->pass[PSS_MAX] = 0;
    sram_off();
}

/* 整条记录清零 (删除; 不留旧密码残迹), WAL 保护记录+CRC+低区校验和. */
static void slot_clear(uint16_t i)
{
    uint16_t a = REC_ADDR(i);
    uint16_t ca = (uint16_t)(PM5_REC_CRC + (uint16_t)i * 2u);
    uint8_t oldrec[REC_SZ], newrec[REC_SZ];
    uint8_t oldc[2], newc[2], oldchk, newchk, j, x = 0;

    rec_read_raw(a, oldrec);
    memset(newrec, 0, REC_SZ);
    sram_on();
    oldc[0] = rb(ca); oldc[1] = rb((uint16_t)(ca + 1));
    oldchk = chk_valid ? vault_chk : 0;
    sram_off();
    for (j = 0; j < REC_SZ; j++) x ^= oldrec[j];
    if (chk_valid) newchk = (uint8_t)(oldchk ^ x);
    else           newchk = oldchk;
    newc[0] = 0; newc[1] = 0;

    wal_reset();
    wal_add(a, REC_SZ, oldrec, newrec);
    wal_add(ca, 2, oldc, newc);
    if (chk_valid) wal_add(OFS_CHK, 1, &oldchk, &newchk);
    wal_commit();

    rec_write_raw(a, newrec);
    sram_on();
    wb(ca, 0); wb((uint16_t)(ca + 1), 0);
    if (chk_valid) wb(OFS_CHK, newchk);
    sram_off();
    vault_chk = newchk;
    if (pm5_active && (bad_slots[i >> 3] & (uint8_t)(1u << (i & 7)))) {
        bad_slots[i >> 3] &= (uint8_t)~(1u << (i & 7));
        if (vault_bad_count) vault_bad_count--;
    }
    wal_clear();
}

/* ------------------------------------------------------------------ */
/* 最近删除回收站 (16 条, 槽位区之后)                                   */
/* ------------------------------------------------------------------ */

static uint8_t bin_magic_ok(void)
{
    return (uint8_t)(rb(BIN_BASE) == 'B' && rb(BIN_BASE + 1) == 'I' &&
                     rb(BIN_BASE + 2) == 'N' && rb(BIN_BASE + 3) == '1');
}

/* 回收站低区 XOR 校验和 (兼容 PMG4) */
static uint8_t bin_chk_calc(void)
{
    uint8_t c = (uint8_t)(0xA5 ^ bin_head);
    uint16_t i;
    for (i = 0; i < BIN_BYTES; i++) c ^= rb((uint16_t)(BIN_DATA + i));
    return c;
}

static uint16_t bin_crc_addr(uint8_t i)
{
    return (uint16_t)(PM5_BIN_CRC + (uint16_t)i * 2u);
}

static uint8_t bin_crc_ok(uint8_t i)
{
    uint8_t ok;
    sram_on();
    ok = (uint8_t)(rd16(bin_crc_addr(i)) ==
                   crc16_sram((uint16_t)(BIN_DATA + (uint16_t)i * REC_SZ), REC_SZ));
    sram_off();
    return ok;
}

static void bin_reset(void)
{
    uint16_t i;
    sram_on();
    for (i = 0; i < BIN_BYTES; i++) wb((uint16_t)(BIN_DATA + i), 0x00);
    for (i = 0; i < BIN_SLOTS * 2u; i++) wb((uint16_t)(PM5_BIN_CRC + i), 0x00);
    bin_head = 0;
    wb(BIN_BASE, 'B'); wb(BIN_BASE + 1, 'I');
    wb(BIN_BASE + 2, 'N'); wb(BIN_BASE + 3, '1');
    wb(BIN_BASE + 4, bin_head);
    wb(BIN_BASE + 5, bin_chk_calc());
    sram_off();
    memset(bad_bin, 0, sizeof(bad_bin));
    bin_ready = 1;
}

static void bin_load(void)
{
    uint8_t ok, i;

    sram_on();
    ok = bin_magic_ok();
    if (ok) {
        bin_head = rb(BIN_BASE + 4);
        if (bin_head >= BIN_SLOTS) ok = 0;
        else if (rb(BIN_BASE + 5) != bin_chk_calc()) ok = 0;
    }
    sram_off();
    if (ok) {
        for (i = 0; i < BIN_SLOTS; i++) {
            sram_on();
            if (rb((uint16_t)(BIN_DATA + (uint16_t)i * REC_SZ)) == USED_MAGIC &&
                !bin_crc_ok(i)) ok = 0;
            sram_off();
            if (!ok) break;
        }
    }

    if (ok) bin_ready = 1;
    else    bin_reset();
}

static void bin_push(const slot_t *s)
{
    uint16_t a;
    uint8_t oldrec[REC_SZ], newrec[REC_SZ];
    uint8_t oldhead, newhead, oldck, newck;
    uint8_t oldc[2], newc[2];
    uint16_t nc;
    uint8_t j;

    if (!bin_ready) bin_load();
    a = (uint16_t)(BIN_DATA + (uint16_t)bin_head * REC_SZ);
    rec_read_raw(a, oldrec);
    rec_pack(s, newrec);
    oldhead = bin_head;
    newhead = (uint8_t)((bin_head + 1) & (BIN_SLOTS - 1));

    sram_on();
    oldck = rb(BIN_BASE + 5);
    oldc[0] = rb(bin_crc_addr(bin_head));
    oldc[1] = rb((uint16_t)(bin_crc_addr(bin_head) + 1));
    sram_off();
    newck = (uint8_t)(oldck ^ oldhead ^ newhead);
    for (j = 0; j < REC_SZ; j++) newck ^= (uint8_t)(oldrec[j] ^ newrec[j]);
    nc = crc16_buf(newrec, REC_SZ);
    newc[0] = (uint8_t)(nc & 0xFF); newc[1] = (uint8_t)(nc >> 8);

    wal_reset();
    wal_add(a, REC_SZ, oldrec, newrec);
    wal_add(BIN_BASE + 4, 1, &oldhead, &newhead);
    wal_add(BIN_BASE + 5, 1, &oldck, &newck);
    wal_add(bin_crc_addr(bin_head), 2, oldc, newc);
    wal_commit();

    rec_write_raw(a, newrec);
    sram_on();
    wb(BIN_BASE + 4, newhead);
    wb(BIN_BASE + 5, newck);
    wb(bin_crc_addr(bin_head), newc[0]);
    wb((uint16_t)(bin_crc_addr(bin_head) + 1), newc[1]);
    sram_off();
    bin_head = newhead;
    wal_clear();
}

static void bin_remove(uint8_t b)
{
    uint16_t a = (uint16_t)(BIN_DATA + (uint16_t)b * REC_SZ);
    uint8_t oldrec[REC_SZ], newrec[REC_SZ];
    uint8_t oldck, newck, oldc[2], newc[2];
    uint8_t j;

    rec_read_raw(a, oldrec);
    memset(newrec, 0, REC_SZ);
    sram_on();
    oldck = rb(BIN_BASE + 5);
    oldc[0] = rb(bin_crc_addr(b));
    oldc[1] = rb((uint16_t)(bin_crc_addr(b) + 1));
    sram_off();
    newck = oldck;
    for (j = 0; j < REC_SZ; j++) newck ^= oldrec[j];
    newc[0] = 0; newc[1] = 0;

    wal_reset();
    wal_add(a, REC_SZ, oldrec, newrec);
    wal_add(BIN_BASE + 5, 1, &oldck, &newck);
    wal_add(bin_crc_addr(b), 2, oldc, newc);
    wal_commit();

    rec_write_raw(a, newrec);
    sram_on();
    wb(BIN_BASE + 5, newck);
    wb(bin_crc_addr(b), 0);
    wb((uint16_t)(bin_crc_addr(b) + 1), 0);
    sram_off();
    wal_clear();
}

/* 迁移/擦除后重建记录 CRC 表 + 回收站 CRC 表; 低区数据不移动. */
static void pm5_build_tables(void)
{
    uint16_t i;
    uint16_t c;
    uint8_t ok;

    memset(bad_slots, 0, sizeof(bad_slots));
    memset(bad_bin, 0, sizeof(bad_bin));
    vault_bad_count = 0;
    damage_ack = 0;
    sram_on();
    for (i = 0; i < N_SLOTS; i++) {
        if (rb(REC_ADDR(i)) == USED_MAGIC) {
            c = crc16_sram(REC_ADDR(i), REC_SZ);
            wr16((uint16_t)(PM5_REC_CRC + i * 2u), c);
        } else {
            wr16((uint16_t)(PM5_REC_CRC + i * 2u), 0);
        }
    }
    sram_off();

    sram_on();
    ok = bin_magic_ok();
    if (ok) {
        bin_head = rb(BIN_BASE + 4);
        if (bin_head >= BIN_SLOTS) ok = 0;
        else if (rb(BIN_BASE + 5) != bin_chk_calc()) ok = 0;
    }
    sram_off();
    if (ok) {
        for (i = 0; i < BIN_SLOTS; i++) {
            sram_on();
            if (rb((uint16_t)(BIN_DATA + i * REC_SZ)) == USED_MAGIC) {
                c = crc16_sram((uint16_t)(BIN_DATA + i * REC_SZ), REC_SZ);
                wr16(bin_crc_addr((uint8_t)i), c);
            } else {
                wr16(bin_crc_addr((uint8_t)i), 0);
            }
            sram_off();
        }
    } else {
        bin_reset();
    }
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
/* 自动锁屏: 解锁后 TIMA@4kHz 计时, 无操作到设定分钟数就回锁库画面     */
/* ------------------------------------------------------------------ */

#define IDLE_WRAPS_PER_MIN 960u   /* 62.5ms × 960 = 60s */

static uint8_t  idle_armed;
static uint8_t  idle_prev;
static uint16_t idle_wraps;
static uint16_t idle_limit;

static void idle_reset(void)
{
    idle_wraps = 0;
    idle_prev = TIMA_REG;
    idle_limit = (uint16_t)((uint16_t)LOCK_MINS[lock_idx] * IDLE_WRAPS_PER_MIN);
}

static void idle_start(void)
{
    TAC_REG = TACF_START | TACF_4KHZ;
    TIMA_REG = 0;
    idle_armed = 1;
    idle_reset();
}

static void idle_stop(void)
{
    idle_armed = 0;
    TAC_REG = TACF_STOP;
    memset(&cur, 0, sizeof(cur));   /* 锁库/回菜单时清掉详情缓存里的明文 */
    pin_idle = 0;
}

/* 返回 1 = 已超时; 任意按键都算活动 */
static uint8_t idle_poll(uint8_t keys)
{
    uint8_t t;

    if (!idle_armed) return 0;
    t = TIMA_REG;
    if (t < idle_prev) idle_wraps++;
    idle_prev = t;
    if (keys) idle_wraps = 0;
    return (uint8_t)(idle_wraps >= idle_limit);
}

/* 忙轮询等待按键; 返回 0 = 自动锁屏 */
static uint8_t ui_getkey(uint8_t mask)
{
    uint8_t prev = 0, j;

    while (1) {
        j = joypad();
        if (idle_poll(j)) return 0;
        if ((j & mask) && !(prev & mask)) {
            waitpadup();
            return (uint8_t)(j & mask);
        }
        prev = j;
    }
}

/* 快速锁库: 非输入画面长按 B 约 1.5 秒; 短按 B 仍然返回.
 * 用 TIMA (4KHz) 非阻塞计时: 24 次溢出 x 62.5ms = 1.5s.
 * 不阻塞当前帧, 便于与输入轮询共存. */
#define B_HOLD_TICKS 24
#define KEY_LOCKED   0xFF

static uint8_t b_hold_active;
static uint8_t b_hold_prev_tima;
static uint8_t b_hold_ticks;

static void b_hold_reset(void)
{
    b_hold_active = 0;
    b_hold_ticks = 0;
    b_hold_prev_tima = TIMA_REG;
}

/* 返回 0=继续, 1=短按 B 释放(返回), 2=长按 B 达到阈值(锁库) */
static uint8_t b_hold_poll(uint8_t j)
{
    uint8_t t = TIMA_REG;

    if (!(j & J_B)) {
        if (b_hold_active) {
            b_hold_active = 0;
            if (b_hold_ticks < B_HOLD_TICKS) return 1;
        }
        b_hold_prev_tima = t;
        return 0;
    }
    if (!b_hold_active) {
        b_hold_active = 1;
        b_hold_ticks = 0;
        b_hold_prev_tima = t;
        return 0;
    }
    if (t < b_hold_prev_tima) b_hold_ticks++;
    b_hold_prev_tima = t;
    if (b_hold_ticks >= B_HOLD_TICKS) {
        b_hold_active = 0;
        return 2;
    }
    return 0;
}

/* 非输入画面的 getkey: B 由 b_hold_poll 统一处理, 其余键走按下沿. */
static uint8_t ui_getkey_noninput(uint8_t mask)
{
    uint8_t prev = 0, j, r;

    b_hold_reset();
    while (1) {
        j = joypad();
        if (idle_poll(j)) return 0;
        r = b_hold_poll(j);
        if (r == 2) return KEY_LOCKED;
        if (r == 1) return J_B;
        if ((j & mask & (uint8_t)~J_B) && !(prev & mask & (uint8_t)~J_B)) {
            waitpadup();
            return (uint8_t)(j & mask & (uint8_t)~J_B);
        }
        prev = j;
    }
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

static void put_tile(uint8_t x, uint8_t y, uint8_t t)
{
    set_bkg_tiles(x, y, 1, 1, &t);
}

/* 9-slice 立体边框 (192..200), 内部保持空白 */
static void draw_frame(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    uint8_t row[20];
    uint8_t i;

    if (w < 3 || h < 3 || x + w > 20 || y + h > 18) return;

    row[0] = 192;
    for (i = 1; i < (uint8_t)(w - 1); i++) row[i] = 193;
    row[w - 1] = 194;
    set_bkg_tiles(x, y, w, 1, row);

    for (i = 1; i < (uint8_t)(h - 1); i++) {
        put_tile(x, (uint8_t)(y + i), 195);
        put_tile((uint8_t)(x + w - 1), (uint8_t)(y + i), 197);
    }

    row[0] = 198;
    for (i = 1; i < (uint8_t)(w - 1); i++) row[i] = 199;
    row[w - 1] = 200;
    set_bkg_tiles(x, (uint8_t)(y + h - 1), w, 1, row);
}

/* 屏幕正中的长方形提示 label: 9-slice 边框 + 居中文字.
 * 先清空整个 8..10 行, 避免残留底层键盘/列表内容贴在边框外. */
static void toast(const char *msg)
{
    uint8_t n = 0, w, x, y;
    while (msg[n] && n < 18) n++;
    w = (uint8_t)(n + 4);
    if (w > 20) w = 20;
    if (w < 8) w = 8;
    x = (uint8_t)((20 - w) >> 1);
    for (y = 8; y <= 10; y++)
        put_fixed(0, y, "", 20, 0);
    draw_frame(x, 8, w, 3);
    put_str((uint8_t)((20 - n) >> 1), 9, msg);
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
static const char GEN_ALL_NS[58] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789"; /* 57 */

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

/* 启动时混入 DIV/LY 与多次按键时序, 避免固定种子.
 * 注意: 这不是密码学安全的随机数, 只用于生成用户可修改的默认密码. */
static void rng_boot(void)
{
    uint8_t i;
    rng_state = (uint16_t)(0xACE1u ^ ((uint16_t)DIV_REG << 8) ^ LY_REG);
    for (i = 0; i < 8; i++) {
        rng_mix((uint16_t)(i * 37u + ((uint16_t)DIV_REG << 3) + LY_REG));
        delay(1);
    }
}

/* 无模偏差采样: 拒绝落在 256 尾部不完整区间的字节 */
static uint8_t rng_below(uint8_t n)
{
    uint16_t limit = (uint16_t)(256u - (256u % (uint16_t)n));
    uint8_t r;
    do {
        r = rng_byte();
    } while ((uint16_t)r >= limit);
    return (uint8_t)(r % n);
}

/* 32 位 xorshift, 只用于生成 12 位恢复码.
 * 种子混入 PIN 输入期间累计的 rng_state + DIV/LY/TIMA, 状态空间远大于
 * 原 16 位 LCG; 真机上人工按键间隔提供额外熵. */
static uint32_t rng32_state;

static uint32_t rng32_next(void)
{
    uint32_t x = rng32_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng32_state = x;
    return x;
}

static void rng32_seed(void)
{
    uint8_t i;
    rng32_state = 0x9E3779B9UL
                ^ ((uint32_t)rng_state << 16)
                ^ ((uint32_t)DIV_REG << 8)
                ^ (uint32_t)LY_REG
                ^ ((uint32_t)TIMA_REG << 24);
    if (!rng32_state) rng32_state = 0x1D2C3B4AUL;
    for (i = 0; i < 4; i++) (void)rng32_next();
}

static uint8_t rng32_below10(void)
{
    uint8_t r;
    do {
        r = (uint8_t)(rng32_next() >> 24);
    } while (r >= 250);
    return (uint8_t)(r % 10);
}

/* 生成强密码: 按设置长度 (12/16/24/32), 可选符号 */
static uint8_t gen_password(char *buf, uint8_t max)
{
    uint8_t n = GEN_LENS[gen_idx];
    uint8_t i, j, t, base;

    if (n > max) n = max;
    rng_mix((uint16_t)(DIV_REG | ((uint16_t)LY_REG << 8)));
    base = 3;
    buf[0] = GEN_UP[rng_below(24)];
    buf[1] = GEN_LO[rng_below(25)];
    buf[2] = GEN_DIG[rng_below(8)];
    if (gen_sym) {
        buf[3] = GEN_SYM[rng_below(13)];
        base = 4;
    }
    for (i = base; i < n; i++)
        buf[i] = gen_sym ? GEN_ALL[rng_below(70)] : GEN_ALL_NS[rng_below(57)];
    for (i = (uint8_t)(n - 1); i > 0; i--) {          /* Fisher-Yates */
        j = rng_below((uint8_t)(i + 1));
        t = (uint8_t)buf[i];
        buf[i] = buf[j];
        buf[j] = (char)t;
    }
    buf[n] = 0;
    return n;
}

/* ------------------------------------------------------------------ */
/* PIN 图形键盘 (2bpp 9-slice)                                          */
/* ------------------------------------------------------------------ */

/* 在开机时生成 201..247 的图形 tile (DISPLAY_OFF 期间写 VRAM) */
static void gen_ui_tiles(void)
{
    volatile uint8_t *vr = (volatile uint8_t *)0x8000;
    uint8_t i, j, b, g, lo, hi, m;
    uint16_t d;

    /* 3x3 PIN 框的中间行: 201..210 = 0..9, 211 = '_' (整字居中) */
    for (i = 0; i < 11; i++) {
        b = SPLIT_SRC[i];
        for (j = 0; j < 8; j++) {
            g = font_tiles[(uint16_t)b * 16 + (uint16_t)j * 2];
            d = (uint16_t)(201 + i) * 16 + (uint16_t)j * 2;
            vr[d] = g;
            vr[d + 1] = (uint8_t)~g;
        }
    }

    /* 选中键整体反白: 212..220 为 FRAME_TILES 的 9-slice 变体,
     * 只把填充色 2 改成黑色 1; 亮边/暗边保持. */
    for (i = 0; i < 9; i++) {
        for (j = 0; j < 8; j++) {
            lo = FRAME_TILES[(uint16_t)i * 16 + (uint16_t)j * 2];
            hi = FRAME_TILES[(uint16_t)i * 16 + (uint16_t)j * 2 + 1];
            m = (uint8_t)(hi & (uint8_t)~lo);
            d = (uint16_t)(212 + i) * 16 + (uint16_t)j * 2;
            vr[d] = (uint8_t)(lo | m);
            vr[d + 1] = (uint8_t)(hi & (uint8_t)~m);
        }
    }

    /* 键帽底色上的字模: 0-9,C,L,R,O,K */
    for (i = 0; i < 15; i++) {
        b = SRC_LABEL[i];
        for (j = 0; j < 8; j++) {
            g = font_tiles[(uint16_t)b * 16 + (uint16_t)j * 2];
            d = (uint16_t)(223 + i) * 16 + (uint16_t)j * 2;
            vr[d] = g;
            vr[d + 1] = (uint8_t)~g;
        }
    }

    /* 标题行: 字模 + 最后一行实心横线 */
    for (i = 0; i < 10; i++) {
        b = SRC_TITLE[i];
        d = (uint16_t)(238 + i) * 16;
        for (j = 0; j < 16; j++)
            vr[d + j] = font_tiles[(uint16_t)b * 16 + j];
        vr[d + 14] = 0xFF;
        vr[d + 15] = 0x00;
    }

    /* 248..255: 小图标 (前景色 1 = 黑) */
    for (i = 0; i < 8; i++) {
        for (j = 0; j < 8; j++) {
            d = (uint16_t)(248 + i) * 16 + (uint16_t)j * 2;
            vr[d] = ICON_BITS[i][j];
            vr[d + 1] = 0;
        }
    }
}

/* 固定标题 SECRET VAULT + 标题下横线 (238=空白+线, 239..247=字母+线) */
static void draw_title(void)
{
    uint8_t row[20];
    uint8_t i;
    for (i = 0; i < 20; i++) row[i] = 238;
    set_bkg_tiles(0, 0, 20, 1, row);
    set_bkg_tiles(4, 0, 12, 1, TITLE_SEQ);
}

/* 一个 3x3 的 PIN 框; center 为中间行中心 tile */
static void pin_box(uint8_t x, uint8_t center)
{
    uint8_t row[3];
    row[0] = 192; row[1] = 193; row[2] = 194;
    set_bkg_tiles(x, 2, 3, 1, row);
    row[0] = 195; row[1] = center; row[2] = 197;
    set_bkg_tiles(x, 3, 3, 1, row);
    row[0] = 198; row[1] = 199; row[2] = 200;
    set_bkg_tiles(x, 4, 3, 1, row);
}

/* 分格 PIN 框: 6 个 (旧档 4 个) 3x3 立体框, 数字整字居中 */
static void pin_field(const uint8_t *out, uint8_t n, uint8_t max)
{
    uint8_t i, x, center;
    x = (uint8_t)((20 - max * 3) >> 1);
    for (i = 0; i < max; i++) {
        center = 196;                           /* 空框: 填充色 */
        if (i < n) {                            /* 已输入数字: 201..210 */
            center = (uint8_t)(201 + out[i]);
        } else if (i == n) {                    /* 当前位: 下划线 211 */
            center = 211;
        }
        pin_box((uint8_t)(x + i * 3), center);
    }
}

/* 一个 6x3 的键: 上/中/下三行立体边框; sel=1 时整键使用反白 9-slice */
static void draw_key(uint8_t x, uint8_t y, uint8_t sel)
{
    uint8_t row[6], b;
    b = sel ? 212 : 192;
    row[0] = b; row[1] = b + 1; row[2] = b + 1; row[3] = b + 1; row[4] = b + 1; row[5] = b + 2;
    set_bkg_tiles(x, y, 6, 1, row);
    row[0] = b + 3; row[1] = b + 4; row[2] = b + 4; row[3] = b + 4; row[4] = b + 4; row[5] = b + 5;
    set_bkg_tiles(x, (uint8_t)(y + 1), 6, 1, row);
    row[0] = b + 6; row[1] = b + 7; row[2] = b + 7; row[3] = b + 7; row[4] = b + 7; row[5] = b + 8;
    set_bkg_tiles(x, (uint8_t)(y + 2), 6, 1, row);
}

/* 键帽标签: 12 个键共 15 个字符 (1..9, CLR, 0, OK) */
static const uint8_t LBL_KEY[15]  = { 0,1,2,3,4,5,6,7,8, 9,9,9, 10, 11,11 };
static const uint8_t LBL_OFF[15]  = { 3,3,3,3,3,3,3,3,3, 2,3,4, 3, 2,3 };
static const uint8_t LBL_NORM[15] = { 224,225,226,227,228,229,230,231,232, 233,234,235, 223, 236,237 };
static const uint8_t LBL_INV[15]  = { 113,114,115,116,117,118,119,120,121, 131,140,146, 112, 143,139 };

static void pad_key(uint8_t idx, uint8_t sel)
{
    uint8_t i, x, y, t;
    x = (uint8_t)(1 + (idx % 3) * 6);
    y = (uint8_t)(5 + (idx / 3) * 3);
    draw_key(x, y, sel);
    for (i = 0; i < 15; i++) {
        if (LBL_KEY[i] != idx) continue;
        t = sel ? LBL_INV[i] : LBL_NORM[i];
        set_bkg_tiles((uint8_t)(x + LBL_OFF[i]), (uint8_t)(y + 1), 1, 1, &t);
    }
}

static void pad_draw_keypad(uint8_t sel)
{
    uint8_t i;
    for (i = 0; i < 12; i++)
        pad_key(i, (uint8_t)(i == sel));
}

/* 输入 max 位 PIN (常规 6 位, 迁移旧档 4 位), 集满或按 OK/START 返回;
 * 返回 0=完成, 1=输入过程中自动锁屏 (仅修改 PIN 时启用),
 * 2=按 SELECT 请求恢复码流程 (仅 unlock 传入 allow_rec=1). */
static uint8_t pin_entry(const char *title, uint8_t *out, uint8_t max,
                         uint8_t allow_rec)
{
    uint8_t n = 0, cx = 0, cy = 0, k, old;
    uint8_t tlen = 0, tx;
    uint8_t mask = J_UP | J_DOWN | J_LEFT | J_RIGHT | J_A | J_B | J_START;

    if (allow_rec) mask |= J_SELECT;
    while (title[tlen] && tlen < 20) tlen++;
    tx = (uint8_t)((20 - (tlen < 20 ? tlen : 20)) >> 1);
    screen_clear();
    draw_title();
    put_str(tx, 1, title);
    pin_field(out, n, max);
    pad_draw_keypad(0);
    put_str(1, 17, allow_rec ? "B=DEL SEL=RECOVERY" : "B=DEL  START=OK");

    while (1) {
        k = pin_idle ? ui_getkey(mask) : getkey(mask);
        if (!k) return 1;
        rng_mix((uint16_t)(k | ((uint16_t)TIMA_REG << 8)));
        if (allow_rec && (k & J_SELECT)) return 2;

        if (k & J_UP) {
            if (cy) {
                old = (uint8_t)(cy * 3 + cx);
                cy--;
                pad_key(old, 0);
                pad_key((uint8_t)(cy * 3 + cx), 1);
            }
            continue;
        }
        if (k & J_DOWN) {
            if (cy < 3) {
                old = (uint8_t)(cy * 3 + cx);
                cy++;
                pad_key(old, 0);
                pad_key((uint8_t)(cy * 3 + cx), 1);
            }
            continue;
        }
        if (k & J_LEFT) {
            if (cx) {
                old = (uint8_t)(cy * 3 + cx);
                cx--;
                pad_key(old, 0);
                pad_key((uint8_t)(cy * 3 + cx), 1);
            }
            continue;
        }
        if (k & J_RIGHT) {
            if (cx < 2) {
                old = (uint8_t)(cy * 3 + cx);
                cx++;
                pad_key(old, 0);
                pad_key((uint8_t)(cy * 3 + cx), 1);
            }
            continue;
        }

        if (k & J_B) {
            if (n) { n--; pin_field(out, n, max); }
            continue;
        }
        if (k & J_A) {
            uint8_t idx = (uint8_t)(cy * 3 + cx);
            if (idx <= 8) {                     /* 1..9 */
                if (n < max) { out[n] = (uint8_t)(idx + 1); n++; pin_field(out, n, max); }
            } else if (idx == 9) {              /* CLR: 清空全部 */
                if (n) { n = 0; pin_field(out, n, max); }
            } else if (idx == 10) {             /* 0 */
                if (n < max) { out[n] = 0; n++; pin_field(out, n, max); }
            } else {                            /* OK */
                if (n == max) return 0;
            }
            if (n == max) return 0;
            continue;
        }
        if (k & J_START) {
            if (n == max) return 0;
        }
    }
}

/* 12 位恢复码显示: 4-4-4 分组, 空位用 '_' */
static void code_field(const uint8_t *out, uint8_t n)
{
    char line[15];
    uint8_t i, p = 0;

    for (i = 0; i < RCV_CODE_LEN; i++) {
        if (i == 4 || i == 8) line[p++] = ' ';
        line[p++] = (i < n) ? (char)('0' + out[i]) : '_';
    }
    line[p] = 0;
    put_str(3, 3, line);
}

/* 输入 12 位恢复码; 集满自动提交. 返回 0=完成, 1=取消.
 * 复用 PIN 图形键盘, 但用单行 4-4-4 字段代替 12 个 3x3 框. */
static uint8_t code_entry(uint8_t *out)
{
    uint8_t n = 0, cx = 0, cy = 0, k, old;

    screen_clear();
    draw_title();
    put_str(3, 1, "RECOVERY CODE");
    put_str(2, 2, "ENTER 12 DIGITS");
    code_field(out, n);
    put_str(2, 4, "B=DEL  START=OK");
    pad_draw_keypad(0);
    put_str(4, 17, "SELECT=BACK");

    while (1) {
        k = getkey(J_UP | J_DOWN | J_LEFT | J_RIGHT | J_A | J_B |
                   J_START | J_SELECT);
        if (!k) return 1;
        rng_mix((uint16_t)(k | ((uint16_t)TIMA_REG << 8)));

        if (k & J_SELECT) return 1;
        if (k & J_UP) {
            if (cy) {
                old = (uint8_t)(cy * 3 + cx);
                cy--;
                pad_key(old, 0);
                pad_key((uint8_t)(cy * 3 + cx), 1);
            }
            continue;
        }
        if (k & J_DOWN) {
            if (cy < 3) {
                old = (uint8_t)(cy * 3 + cx);
                cy++;
                pad_key(old, 0);
                pad_key((uint8_t)(cy * 3 + cx), 1);
            }
            continue;
        }
        if (k & J_LEFT) {
            if (cx) {
                old = (uint8_t)(cy * 3 + cx);
                cx--;
                pad_key(old, 0);
                pad_key((uint8_t)(cy * 3 + cx), 1);
            }
            continue;
        }
        if (k & J_RIGHT) {
            if (cx < 2) {
                old = (uint8_t)(cy * 3 + cx);
                cx++;
                pad_key(old, 0);
                pad_key((uint8_t)(cy * 3 + cx), 1);
            }
            continue;
        }
        if (k & J_B) {
            if (n) { n--; code_field(out, n); }
            continue;
        }
        if (k & J_A) {
            uint8_t idx = (uint8_t)(cy * 3 + cx);
            if (idx <= 8) {
                if (n < RCV_CODE_LEN) {
                    out[n] = (uint8_t)(idx + 1);
                    n++;
                    code_field(out, n);
                }
            } else if (idx == 9) {
                if (n) { n = 0; code_field(out, n); }
            } else if (idx == 10) {
                if (n < RCV_CODE_LEN) {
                    out[n] = 0;
                    n++;
                    code_field(out, n);
                }
            }
            if (n == RCV_CODE_LEN) return 0;
            continue;
        }
        if (k & J_START) {
            if (n == RCV_CODE_LEN) return 0;
        }
    }
}

static void recovery_generate(uint8_t *code)
{
    uint8_t i;
    rng32_seed();
    for (i = 0; i < RCV_CODE_LEN; i++)
        code[i] = rng32_below10();
}

/* 生成并显示 12 位恢复码; A=保存 hash, B=取消(不保存).
 * 返回 1 = 自动锁屏, 0 = 已保存或取消. */
static uint8_t recovery_create_ui(void)
{
    uint8_t code[RCV_CODE_LEN], hash[RCV_HASH_LEN], k;
    char line[15];
    uint8_t i, p = 0;

    recovery_generate(code);
    for (i = 0; i < RCV_CODE_LEN; i++) {
        if (i == 4 || i == 8) line[p++] = ' ';
        line[p++] = (char)('0' + code[i]);
    }
    line[p] = 0;

    screen_clear();
    draw_frame(0, 1, 20, 15);
    put_str(3, 3, "YOUR RECOVERY");
    put_str(4, 4, "CODE");
    put_str(3, 6, line);
    put_str(1, 8, "WRITE IT DOWN NOW");
    put_str(1, 9, "IT WILL NOT SHOW");
    put_str(1, 10, "AGAIN");
    put_str(2, 13, "A=SAVE  B=CANCEL");

    k = ui_getkey_noninput(J_A | J_B);
    if (!k || k == KEY_LOCKED) return 1;
    if (k & J_B) return 0;

    recovery_hash(code, hash);
    pm5_rcv_save(hash);
    toast("CODE SAVED");
    return 0;
}

/* 首次设置/迁移完成后的可选恢复码提示 */
static void recovery_offer(void)
{
    uint8_t k;

    screen_clear();
    draw_frame(1, 2, 18, 12);
    put_str(2, 4, "OPTIONAL SAFETY");
    put_str(4, 5, "ADD 12-DIGIT");
    put_str(3, 6, "RECOVERY CODE?");
    put_str(3, 8, "USE IF PIN IS");
    put_str(5, 9, "FORGOTTEN");
    put_str(3, 11, "A=YES  B=SKIP");

    k = getkey(J_A | J_B);
    if (k & J_A) (void)recovery_create_ui();
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

/* 返回输入长度; gen=0 时 SELECT = 取消(-1); gen=1 时 SELECT = 生成随机密码.
 * init != 0 时预填 (编辑已有条目); allow_empty=1 时允许空内容 (用户名). */
static int16_t edit_field(const char *title, char *buf, uint8_t max, uint8_t gen,
                          const char *init, uint8_t allow_empty)
{
    uint8_t len = 0, row = 0, col = 0, k;
    uint16_t tick = 0;

    kb_mode = KB_UPPER;
    if (init) {
        while (init[len] && len < max) { buf[len] = init[len]; len++; }
    }
    buf[len] = 0;
    draw_edit(title, buf, len, row, col, gen);

    while (1) {
        tick++;
        k = ui_getkey(J_UP | J_DOWN | J_LEFT | J_RIGHT | J_A | J_B | J_START | J_SELECT);
        if (!k) return -2;                 /* 自动锁屏 */
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
            if (len || allow_empty) return (int16_t)len;
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
                if (len || allow_empty) return (int16_t)len;
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

#define LIST_LABEL_W 14
#define LIST_ROW_W   18
#define LIST_PAGE    12

/* 跑马灯节拍: TIMA 4kHz 每次回绕 62.5ms */
#define SCROLL_LEAD  8      /* 起始停顿 ~0.5s */
#define SCROLL_HOLD 12      /* 到两端停顿 ~0.75s */
#define SCROLL_STEP  2      /* 每格 ~0.125s */

/* 按住上下连续滚动 */
#define HOLD_LEAD    6      /* ~0.375s 后开始连发 */
#define HOLD_STEP    2      /* 每 ~0.125s 走一格 */

/* "001 abcdefghijklmn" (18 字符): 3 位编号 + 空格 + 14 字符显示窗 */
static void slot_row_text(uint16_t idx, char *out, uint8_t off)
{
    slot_t s;
    uint8_t j, k, n;
    const char *src;

    slot_read(idx, &s);
    fmt3_zp(out, (uint16_t)(idx + 1));
    out[3] = ' ';
    if (s.used) {
        if (list_user) {
            src = s.user;
            n = s.ulen;
            if (!n) { src = "-"; n = 1; }
        } else {
            src = s.label;
            n = s.llen;
        }
        for (j = 0; j < LIST_LABEL_W; j++) {
            k = (uint8_t)(off + j);
            out[4 + j] = (k < n) ? src[k] : ' ';
        }
    } else {
        static const char *e = "EMPTY";
        for (j = 0; j < LIST_LABEL_W; j++) out[4 + j] = e[j] ? e[j] : ' ';
    }
    out[4 + LIST_LABEL_W] = 0;
}

static void draw_slot_list(const char *title)
{
    char row[20], pos[8];
    uint8_t r;
    uint16_t idx;

    screen_clear();
    put_str(1, 0, title);
    fmt_pos(pos, (uint16_t)(list_sel + 1));
    put_str(13, 0, pos);
    put_str(9, 1, "HOLD B=LOCK");
    draw_frame(0, 2, 20, 14);

    for (r = 0; r < LIST_ROWS; r++) {
        idx = (uint16_t)(list_view + r);
        if (idx < N_SLOTS) {
            slot_row_text(idx, row, 0);
            if (idx == list_sel) put_fixed(1, (uint8_t)(LIST_TOP + r), row, LIST_ROW_W, 1);
            else                 put_str(1, (uint8_t)(LIST_TOP + r), row);
        } else {
            put_fixed(1, (uint8_t)(LIST_TOP + r), "", LIST_ROW_W, 0);
        }
    }
    put_str(1, 16, list_user ? "L/R=PAGE SEL=LABEL" : "L/R=PAGE SEL=USER");
    put_str(1, 17, "A=OPEN B=BACK");
}

/* 只重绘高亮行 (跑马灯每一步) */
static void draw_sel_row(uint8_t off)
{
    char row[20];
    uint8_t r = (uint8_t)(list_sel - list_view);
    slot_row_text(list_sel, row, off);
    put_fixed(1, (uint8_t)(LIST_TOP + r), row, LIST_ROW_W, 1);
}

static uint8_t sel_text_len(void)
{
    slot_t s;
    slot_read(list_sel, &s);
    if (!s.used) return 0;
    if (list_user) return s.ulen ? s.ulen : 1;
    return s.llen;
}

static void draw_pass_line(uint8_t reveal)
{
    char line[19];
    uint8_t j, off, n = cur.plen;
    for (off = 0; off < 2; off++) {
        for (j = 0; j < 18; j++) {
            uint8_t idx = (uint8_t)(off * 18 + j);
            line[j] = (idx < n) ? (reveal ? cur.pass[idx] : '*') : ' ';
        }
        line[18] = 0;
        put_str(1, (uint8_t)(7 + off), line);
    }
}

/* 详情页字段宽 18; 超长时截断并显示 '>' 提示, 原数据不受影响. */
static void put_detail_value(uint8_t y, const char *s)
{
    char tmp[19];
    uint8_t i, n = (uint8_t)strlen(s);

    if (n <= 18) {
        put_fixed(1, y, s, 18, 0);
        return;
    }
    for (i = 0; i < 17; i++) tmp[i] = s[i];
    tmp[17] = '>';
    tmp[18] = 0;
    put_fixed(1, y, tmp, 18, 0);
}

static void draw_detail(uint16_t i)
{
    char t[13];
    screen_clear();
    fmt_slot_title(t, (uint16_t)(i + 1));
    put_str(1, 0, t);
    draw_frame(0, 1, 20, 14);
    put_fixed(1, 2, " LABEL:", 7, 1);
    put_tile(18, 2, 248);
    put_detail_value(3, cur.label);
    put_fixed(1, 4, " USER:", 6, 1);
    put_tile(18, 4, 250);
    put_detail_value(5, cur.user);
    put_fixed(1, 6, " PASS:", 6, 1);
    put_tile(18, 6, 251);
    draw_pass_line(0);
    put_str(1, 15, "A=REVEAL B=BACK");
    put_str(1, 16, "SEL+START=EDIT");
    put_str(1, 17, "START=DELETE");
}

/* 编辑已有条目: 三个字段均预填, 可逐页修改; 返回 1 = 自动锁屏 */
static uint8_t edit_flow(uint16_t i)
{
    char lab[LBL_MAX + 1], usr[USR_MAX + 1], pw[PSS_MAX + 1];
    slot_t s;
    int16_t r;

    slot_read(i, &s);
    if (!s.used) return 0;

    r = edit_field("LABEL (MAX 20)", lab, LBL_MAX, 0, s.label, 0);
    if (r < 0) return (uint8_t)(r == -2);
    r = edit_field("USERNAME (MAX 20)", usr, USR_MAX, 0, s.user, 1);
    if (r < 0) return (uint8_t)(r == -2);
    r = edit_field("PASSWORD (MAX 36)", pw, PSS_MAX, 1, s.pass, 0);
    if (r < 0) return (uint8_t)(r == -2);

    memset(s.label, 0, sizeof(s.label));
    s.llen = (uint8_t)strlen(lab);
    memcpy(s.label, lab, s.llen);
    memset(s.user, 0, sizeof(s.user));
    s.ulen = (uint8_t)strlen(usr);
    memcpy(s.user, usr, s.ulen);
    memset(s.pass, 0, sizeof(s.pass));
    s.plen = (uint8_t)strlen(pw);
    memcpy(s.pass, pw, s.plen);

    slot_write(i, &s);
    vault_commit();
    toast("UPDATED");
    return 0;
}

static uint8_t detail_screen(uint16_t i)
{
    uint8_t prev = 0, reveal = 0, pressed, k, r;

    slot_read(i, &cur);
    if (!cur.used) return 0;

    b_hold_reset();
    draw_detail(i);
    /* 纯 joypad 忙轮询 (每帧数千次): 不依赖 HALT/中断帧同步,
     * 否则模拟器 (serverboy) 的 HALT 唤醒会被按键事件打断, 点按会丢。 */
    while (1) {
        uint8_t j = joypad();
        pressed = (uint8_t)(j & ~prev);

        if (idle_poll(j)) return 1;
        r = b_hold_poll(j);
        if (r == 2) return 1;
        if (r == 1) return 0;

        if ((j & J_A) && !reveal) { reveal = 1; draw_pass_line(1); }
        else if (!(j & J_A) && reveal) { reveal = 0; draw_pass_line(0); }

        /* 编辑组合键必须放在 START 删除之前判断 */
        if ((j & (J_SELECT | J_START)) == (J_SELECT | J_START)) {
            waitpadup();
            if (edit_flow(i)) return 1;
            slot_read(i, &cur);
            draw_detail(i);
            reveal = 0;
            prev = joypad();
            continue;
        }

        if (pressed & J_START) {
            put_str(1, 13, "DELETE THIS SLOT?");
            put_str(1, 14, "A=YES  B=NO");
            waitpadup();
            k = ui_getkey(J_A | J_B);
            if (!k) return 1;
            if (k & J_A) {
                bin_push(&cur);
                slot_clear(i);
                vault_commit();
                toast("DELETED");
                waitpadup();
                return 0;
            }
            draw_detail(i);
            reveal = 0;
        }
        prev = j;
    }
}

/* 返回 1 = 自动锁屏 */
static uint8_t add_flow(int16_t slot)
{
    char lab[LBL_MAX + 1];
    char usr[USR_MAX + 1];
    char pw[PSS_MAX + 1];
    slot_t s;
    int16_t r;

    if (slot < 0) {                 /* 自动写入第一个空槽 */
        slot = find_empty();
        if (slot < 0) { toast("VAULT FULL"); return 0; }
    }

    r = edit_field("LABEL (MAX 20)", lab, LBL_MAX, 0, 0, 0);
    if (r < 0) return (uint8_t)(r == -2);

    r = edit_field("USERNAME (MAX 20)", usr, USR_MAX, 0, 0, 1);
    if (r < 0) return (uint8_t)(r == -2);

    r = edit_field("PASSWORD (MAX 36)", pw, PSS_MAX, 1, 0, 0);
    if (r < 0) return (uint8_t)(r == -2);

    memset(&s, 0, sizeof(slot_t));
    s.used = 1;
    s.llen = (uint8_t)strlen(lab);
    memcpy(s.label, lab, s.llen);
    s.ulen = (uint8_t)strlen(usr);
    memcpy(s.user, usr, s.ulen);
    s.plen = (uint8_t)strlen(pw);
    memcpy(s.pass, pw, s.plen);

    slot_write((uint16_t)slot, &s);
    vault_commit();
    toast("SAVED");
    return 0;
}

static uint8_t list_screen(void)
{
    uint8_t prev = 0, off = 0, dir = 1, sel_len, delay_left, step_div = 0;
    uint8_t prev_tima, hold = 0, hold_wait = 0, hold_div = 0, r;

    list_ensure_visible();
    b_hold_reset();
    draw_slot_list("ACCOUNTS");
    sel_len = sel_text_len();
    delay_left = SCROLL_LEAD;

    /* TIMA 由主循环的自动锁屏计时器启动; 这里只读时基 */
    prev_tima = TIMA_REG;

    while (1) {
        uint8_t j = joypad();
        uint8_t pressed = (uint8_t)(j & ~prev);
        uint8_t redraw = 0;

        if (idle_poll(j)) return 1;
        r = b_hold_poll(j);
        if (r == 2) return 1;
        if (r == 1) return 0;
        prev = j;
        if ((hold == 1 && !(j & J_UP)) || (hold == 2 && !(j & J_DOWN)))
            hold = 0;

        if (pressed & J_START) return 0;

        if (pressed & J_SELECT) {               /* 标签 / 用户名视图切换 */
            list_user = (uint8_t)(!list_user);
            settings_save();
            hold = 0;
            redraw = 1;
        } else if (pressed & J_A) {
            if (slot_used(list_sel)) {
                if (detail_screen(list_sel)) return 1;
            } else if (slot_raw_used(list_sel)) {
                toast("SLOT DAMAGED");
            } else {
                if (add_flow((int16_t)list_sel)) return 1;
            }
            prev = joypad();    /* 子画面返回时仍按住的键不算新按下 */
            hold = 0;
            redraw = 1;
        } else if (pressed & J_UP) {
            if (list_sel) {
                list_sel--;
                hold = 1; hold_wait = HOLD_LEAD; hold_div = 0;
            }
            redraw = 1;
        } else if (pressed & J_DOWN) {
            if (list_sel < (uint16_t)(N_SLOTS - 1)) {
                list_sel++;
                hold = 2; hold_wait = HOLD_LEAD; hold_div = 0;
            }
            redraw = 1;
        } else if (pressed & J_LEFT) {          /* 向左翻一页 */
            list_sel = (list_sel >= LIST_PAGE) ? (uint16_t)(list_sel - LIST_PAGE) : 0;
            hold = 0;
            redraw = 1;
        } else if (pressed & J_RIGHT) {         /* 向右翻一页 */
            list_sel = (uint16_t)((list_sel + LIST_PAGE <= N_SLOTS - 1)
                                  ? list_sel + LIST_PAGE : N_SLOTS - 1);
            hold = 0;
            redraw = 1;
        }

        if (redraw) {
            off = 0; dir = 1; delay_left = SCROLL_LEAD; step_div = 0;
            list_ensure_visible();
            draw_slot_list("ACCOUNTS");
            sel_len = sel_text_len();
            prev_tima = TIMA_REG;
            continue;
        }

        /* TIMA 8 位回绕 = 62.5ms: 按住连发 + 跑马灯共用时基 */
        {
            uint8_t t = TIMA_REG;
            if (t != prev_tima) {
                if (t < prev_tima) {
                    if (hold) {
                        if (hold_wait) {
                            hold_wait--;
                        } else if (++hold_div >= HOLD_STEP) {
                            hold_div = 0;
                            if (hold == 1) {
                                if (list_sel) list_sel--;
                                else hold = 0;
                            } else {
                                if (list_sel < (uint16_t)(N_SLOTS - 1)) list_sel++;
                                else hold = 0;
                            }
                            if (hold || list_sel) {
                                off = 0; dir = 1; delay_left = SCROLL_LEAD;
                                list_ensure_visible();
                                draw_slot_list("ACCOUNTS");
                                sel_len = sel_text_len();
                                prev_tima = t;
                                continue;
                            }
                        }
                    }
                    if (sel_len > LIST_LABEL_W) {
                        if (delay_left) {
                            delay_left--;
                        } else if (++step_div >= SCROLL_STEP) {
                            uint8_t maxoff = (uint8_t)(sel_len - LIST_LABEL_W);
                            step_div = 0;
                            if (dir) {
                                off++;
                                if (off >= maxoff) {
                                    off = maxoff;
                                    dir = 0;
                                    delay_left = SCROLL_HOLD;
                                }
                            } else {
                                if (off) off--;
                                if (!off) { dir = 1; delay_left = SCROLL_LEAD; }
                            }
                            draw_sel_row(off);
                        }
                    }
                }
                prev_tima = t;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* 最近删除回收站界面                                                  */
/* ------------------------------------------------------------------ */

static uint8_t bin_screen(void)
{
    uint8_t idx[BIN_SLOTS];
    uint8_t n, sel = 0, r, i, k;
    char row[20];
    slot_t s;

    if (!bin_ready) bin_load();

    while (1) {
        n = 0;
        sram_on();
        for (i = 0; i < BIN_SLOTS; i++) {
            uint8_t b = (uint8_t)((bin_head + BIN_SLOTS - 1 - i) & (BIN_SLOTS - 1));
            if (rb((uint16_t)(BIN_DATA + (uint16_t)b * REC_SZ)) == USED_MAGIC)
                idx[n++] = b;
        }
        sram_off();
        if (n && sel >= n) sel = (uint8_t)(n - 1);

        screen_clear();
        put_tile(0, 0, 252);
        put_fixed(1, 0, " RECYCLE BIN ", 18, 1);
        for (r = 0; r < BIN_SLOTS; r++) {
            if (r < n) {
                rec_read_at((uint16_t)(BIN_DATA + (uint16_t)idx[r] * REC_SZ), &s);
                row[0] = (char)('0' + (r + 1) / 10);
                row[1] = (char)('0' + (r + 1) % 10);
                row[2] = ' ';
                for (i = 0; i < LIST_LABEL_W; i++)
                    row[3 + i] = (i < s.llen) ? s.label[i] : ' ';
                row[3 + LIST_LABEL_W] = ' ';
                row[4 + LIST_LABEL_W] = 0;
                put_fixed(1, (uint8_t)(1 + r), row, 18, (uint8_t)(r == sel));
            } else {
                put_fixed(1, (uint8_t)(1 + r), "", 18, 0);
            }
        }
        if (!n) {
            put_tile(9, 3, 252);
            put_str(3, 5, "BIN EMPTY");
            put_str(1, 17, "B=BACK (HOLD=LOCK)");
        } else {
            put_str(1, 16, "A=RESTORE SEL=DEL");
            put_str(1, 17, "START=EMPTY B=BACK");
        }

        k = ui_getkey_noninput(J_UP | J_DOWN | J_A | J_B | J_SELECT | J_START);
        if (!k) return 1;
        if (k == KEY_LOCKED) return 1;
        if (k & J_B) return 0;
        if (!n) continue;
        if (k & J_UP)   { if (sel) sel--; continue; }
        if (k & J_DOWN) { if (sel + 1 < n) sel++; continue; }

        if (k & J_SELECT) {             /* 彻底删除单条 */
            screen_clear();
            put_str(1, 4, "DELETE THIS ITEM?");
            put_str(1, 5, "THIS CANNOT BE UNDONE");
            put_str(1, 8, "A=YES  B=NO");
            k = ui_getkey(J_A | J_B);
            if (!k) return 1;
            if (k & J_A) {
                bin_remove(idx[sel]);
                if (sel) sel--;
                toast("DELETED");
            }
            continue;
        }

        if (k & J_START) {              /* 清空整个回收站 */
            screen_clear();
            put_str(1, 3, "EMPTY RECYCLE BIN?");
            put_str(1, 5, "THIS CANNOT BE UNDONE");
            put_str(1, 8, "A=YES  B=NO");
            k = ui_getkey(J_A | J_B);
            if (!k) return 1;
            if (k & J_A) {
                bin_reset();
                sel = 0;
                toast("BIN EMPTY");
            }
            continue;
        }

        if (k & J_A) {
            int16_t t;
            rec_read_at((uint16_t)(BIN_DATA + (uint16_t)idx[sel] * REC_SZ), &s);
            t = find_empty();
            if (t < 0) {
                toast("VAULT FULL");
            } else {
                slot_write((uint16_t)t, &s);
                vault_commit();
                bin_remove(idx[sel]);
                toast("RESTORED");
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* 解锁 / 初始化 / 改 PIN / 锁定彩蛋                                    */
/* ------------------------------------------------------------------ */

/* 返回 1 = 用户按 SELECT 请求恢复码流程; 0 = 已擦除 (或不允许恢复).
 * allow_recovery=0 用于 v0.3 迁移的 4 位旧 PIN, 此时没有恢复码. */
static uint8_t locked_screen(uint8_t allow_recovery)
{
    uint8_t wraps = 0;
    uint8_t prev_tima, prev = 0;

    screen_clear();
    put_str(2, 4, "SYSTEM LOCKED");
    put_str(2, 6, "VAULT SEALED");
    if (allow_recovery) put_str(2, 9, "SELECT=RECOVERY");
    else                put_str(0, 9, "RECOVERY UNAVAILABLE");
    put_str(1, 11, "HOLD A+B+SEL+START");
    put_str(1, 12, "3 SECONDS TO WIPE");
    put_str(1, 14, "OR POWER OFF");
    put_str(0, 16, "WIPE ERASES ALL DATA");
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

        /* SELECT 单独按下 = 进入恢复码输入; 不干扰四键擦除组合. */
        if (allow_recovery && (j & J_SELECT) &&
            !(j & (J_A | J_B | J_START)) && !(prev & J_SELECT)) {
            TAC_REG = TACF_STOP;
            waitpadup();
            return 1;
        }

        if ((j & (J_A | J_B | J_SELECT | J_START)) == (J_A | J_B | J_SELECT | J_START)) {
            if (t < prev_tima) {            /* TIMA 8 位回绕 = 256 tick */
                if (++wraps >= 48) {
                    TAC_REG = TACF_STOP;    /* 先关定时器再擦除 */
                    vault_wipe();
                    toast("VAULT WIPED");
                    return 0;
                }
            }
        } else {
            wraps = 0;
        }
        prev_tima = t;
        prev = j;
    }
}

/* 只读扫描: 统计 used 标记且长度字段合理的记录, 不写 SRAM */
static uint16_t scan_used_records(void)
{
    uint16_t i, n = 0;
    uint8_t llen, plen;

    sram_on();
    for (i = 0; i < N_SLOTS; i++) {
        uint16_t a = REC_ADDR(i);
        if (rb(a) != USED_MAGIC) continue;
        llen = rb((uint16_t)(a + REC_OFS_LLEN));
        plen = rb((uint16_t)(a + REC_OFS_PLEN));
        if (llen <= LBL_MAX && plen <= PSS_MAX) n++;
    }
    sram_off();
    return n;
}

/* 损坏/未知存档恢复画面. 返回 1 = 重试读取, 0 = 已明确擦除 */
static uint8_t recovery_screen(uint8_t kind)
{
    char count[8];
    uint16_t valid = 0;
    uint8_t k;

    if (kind == LOAD_CORRUPT) valid = scan_used_records();
    if (kind == LOAD_DAMAGED) valid = vault_bad_count;

    while (1) {
        screen_clear();
        draw_frame(0, 0, 20, 17);
        put_tile(1, 1, 255);
        if (kind == LOAD_DAMAGED) {
            put_str(1, 2, "SAVE DAMAGED");
            fmt3_zp(count, valid);
            put_str(1, 4, "BAD RECORDS:");
            put_str(13, 4, count);
            put_str(1, 6, "OTHER DATA INTACT");
            put_str(1, 10, "A=CONTINUE");
        } else {
            put_str(1, 2, (kind == LOAD_UNSUPPORTED)
                            ? "UNSUPPORTED SAVE" : "SAVE CORRUPT");
            put_str(1, 4, "DATA IS UNTOUCHED");
            if (kind == LOAD_CORRUPT) {
                fmt3_zp(count, valid);
                count[3] = '/'; count[4] = '2'; count[5] = '5'; count[6] = '5'; count[7] = 0;
                put_str(1, 6, "SCAN FINDS");
                put_str(12, 6, count);
            } else {
                put_str(1, 6, "UNKNOWN/OLD FORMAT");
            }
            put_str(1, 10, "A=RETRY");
        }
        put_str(1, 11, "START=WIPE");
        put_str(1, 14, "WIPE ERASES ALL");
        put_str(1, 15, "ONLY IF YOU ACCEPT");

        k = ui_getkey(J_A | J_START);
        if (k & J_A) return (uint8_t)((kind == LOAD_DAMAGED) ? 2 : 1);

        if (k & J_START) {
            screen_clear();
            draw_frame(0, 0, 20, 17);
            put_str(2, 3, "ERASE ALL DATA?");
            put_str(2, 5, "CANNOT BE UNDONE");
            put_str(2, 8, "A=YES  B=NO");
            k = ui_getkey(J_A | J_B);
            if (k & J_A) {
                vault_wipe();
                toast("VAULT WIPED");
                return 0;
            }
        }
    }
}

static uint8_t pin_ok(const uint8_t *in)
{
    uint8_t i;
    for (i = 0; i < PIN_LEN; i++) {
        if (((in[i] + 7) % 10) != pin_obf[i]) return 0;
    }
    return 1;
}

/* 新 PIN 输入两次并写入混淆值 (不含 header_save); 返回 1 = 自动锁屏 */
static uint8_t choose_pin(void)
{
    uint8_t a[PIN_LEN], b[PIN_LEN], i, same;

    while (1) {
        if (pin_entry("NEW PIN", a, PIN_LEN, 0)) return 1;
        if (pin_entry("CONFIRM PIN", b, PIN_LEN, 0)) return 1;
        same = 1;
        for (i = 0; i < PIN_LEN; i++) {
            if (a[i] != b[i]) { same = 0; break; }
        }
        if (same) break;
        toast("PIN MISMATCH");
    }
    for (i = 0; i < PIN_LEN; i++) pin_obf[i] = (uint8_t)((a[i] + 7) % 10);
    return 0;
}

/* 恢复码验证通过后重设 PIN; 返回 1 = 成功解锁/新 PIN 已保存. */
static uint8_t recovery_reset_flow(void)
{
    uint8_t code[RCV_CODE_LEN], attempts = 0;

    if (!rcv_set) {
        toast("NO RECOVERY CODE");
        return 0;
    }
    while (1) {
        if (code_entry(code)) return 0;
        if (pm5_rcv_verify(code)) break;
        toast("WRONG CODE");
        if (++attempts >= 3) return 0;
    }

    screen_clear();
    put_str(3, 4, "CODE ACCEPTED");
    put_str(3, 6, "SET A NEW PIN");
    delay(900);
    pin_idle = 0;
    if (choose_pin()) return 0;
    pin_set = 1;
    tries = 0;
    header_save();
    toast("PIN RESET");
    return 1;
}

/* 三次错误后的锁定画面; 已设置恢复码时 SELECT 可进入恢复流程. */
static uint8_t locked_recovery_loop(void)
{
    while (1) {
        if (!locked_screen(rcv_set)) return 0;
        if (recovery_reset_flow()) return 1;
    }
}

static uint8_t unlock(void)
{
    uint8_t in[PIN_LEN], r;

    if (tries >= 3) return locked_recovery_loop();
    while (1) {
        r = pin_entry("ENTER PIN", in, PIN_LEN, rcv_set);
        if (r == 2) {
            if (recovery_reset_flow()) return 1;
            continue;
        }
        if (r == 1) return 0;
        if (pin_ok(in)) {
            if (tries) { tries = 0; header_save(); }
            return 1;
        }
        tries++;
        header_save();
        if (tries >= 3) return locked_recovery_loop();
        toast("WRONG PIN");
    }
}

static void setup_pin(void)
{
    screen_clear();
    put_str(2, 1, "FIRST TIME SETUP");
    put_str(1, 4, "CHOOSE 6-DIGIT PIN");
    put_str(1, 5, "IT UNLOCKS THE VAULT");
    delay(1500);

    choose_pin();
    pin_set = 1;
    tries = 0;
    pm5_active = 1;
    vault_clear_slots();            /* 首次设置: 清空全部槽位 */
    pm5_build_tables();
    header_save();
    toast("PIN SAVED");
    recovery_offer();
}

/* 返回 1 = 输入过程中自动锁屏 (新 PIN 未保存) */
static uint8_t change_pin(void)
{
    uint8_t old[PIN_LEN];

    pin_idle = 1;
    if (pin_entry("OLD PIN", old, PIN_LEN, 0)) {
        pin_idle = 0;
        return 1;
    }
    if (!pin_ok(old)) {
        pin_idle = 0;
        toast("WRONG PIN");
        return 0;
    }
    if (choose_pin()) {
        pin_idle = 0;
        return 1;
    }
    pin_idle = 0;
    header_save();
    toast("PIN UPDATED");
    return 0;
}

/* ------------------------------------------------------------------ */
/* v0.3 (PMG3) 存档一次性迁移                                          */
/* ------------------------------------------------------------------ */

static uint8_t legacy_pin_ok(const uint8_t *in)
{
    uint8_t i;
    for (i = 0; i < 4; i++) {
        if (((in[i] + 7) % 10) != legacy_pin_obf[i]) return 0;
    }
    return 1;
}

/* 迁移期间也持久化旧档错误次数 (旧 CHK 跳过字节 9) */
static void legacy_tries_save(void)
{
    sram_on();
    wb(8, legacy_tries);
    wb(9, legacy_chk_calc());
    sram_off();
}

static uint8_t legacy_unlock(void)
{
    uint8_t in[4];

    if (legacy_tries >= 3) {
        locked_screen(0);
        return 0;
    }
    while (1) {
        pin_entry("OLD 4-DIGIT PIN", in, 4, 0);
        if (legacy_pin_ok(in)) {
            if (legacy_tries) { legacy_tries = 0; legacy_tries_save(); }
            return 1;
        }
        legacy_tries++;
        legacy_tries_save();
        if (legacy_tries >= 3) {
            locked_screen(0);
            return 0;
        }
        toast("WRONG PIN");
    }
}

/* 返回 1 = 迁移成功; 0 = 已擦除/放弃 (调用方走首次设置) */
static uint8_t migrate_v03(void)
{
    uint16_t i;
    slot_t s;

    screen_clear();
    put_str(1, 2, "V0.3 SAVE FOUND");
    put_str(1, 4, "ONE-TIME UPGRADE");
    put_str(1, 6, "OLD 4-DIGIT PIN");
    put_str(1, 7, "REQUIRED");
    delay(1600);

    if (!legacy_unlock()) return 0;

    screen_clear();
    put_str(1, 3, "CHOOSE NEW");
    put_str(1, 5, "6-DIGIT PIN");
    delay(1300);
    choose_pin();

    screen_clear();
    put_str(1, 2, "UPGRADING SAVE");
    put_str(1, 4, "DO NOT POWER OFF");
    put_str(1, 6, "PLEASE WAIT...");

    /* 阶段 1: 旧档 -> 空闲高区暂存 (旧数据完全不动, 断电可重来) */
    for (i = 0; i < N_SLOTS; i++) {
        legacy_slot_read(i, &s);
        rec_write_at((uint16_t)(MIG_TMP + i * REC_SZ), &s);
    }

    /* 阶段 2: 暂存区 -> 正式位置 (升序; 目标始终低于未读源) */
    for (i = 0; i < N_SLOTS; i++) {
        rec_read_at((uint16_t)(MIG_TMP + i * REC_SZ), &s);
        rec_write_at(REC_ADDR(i), &s);
    }

    /* 清掉暂存区里的密码副本 (其中也包含 PMG5 高区, 必须最后重建) */
    sram_on();
    for (i = 0; i < (uint16_t)(N_SLOTS * REC_SZ); i++) {
        wb((uint16_t)(MIG_TMP + i), 0x00);
    }
    sram_off();
    rcv_cache_clear();              /* 暂存区清零时高区恢复块也被清除 */

    pin_set = 1;
    tries = 0;
    pm5_active = 1;
    pm5_build_tables();
    header_save();                  /* PMG4 兼容低头 + PMG5 高头 */

    screen_clear();
    put_str(2, 4, "SAVE UPGRADED");
    put_str(1, 6, "V0.3.7.1 READY");
    delay(1800);
    recovery_offer();
    return 1;
}

/* PMG4 -> PMG5: 只增加 CRC 表/双头, 不移动槽位, 断电可重复执行. */
static uint8_t migrate_v04(void)
{
    screen_clear();
    put_str(1, 2, "PMG4 SAVE FOUND");
    put_str(1, 4, "UPGRADING SAVE");
    put_str(1, 6, "DO NOT POWER OFF");
    put_str(1, 8, "PLEASE WAIT...");

    pm5_build_tables();
    pm5_active = 1;
    header_save();

    screen_clear();
    put_str(2, 4, "SAVE UPGRADED");
    put_str(1, 6, "V0.3.7.1 READY");
    delay(1200);
    return 1;
}

/* 设置页的恢复码子界面: 创建 / 替换 / 移除. 返回 1 = 自动锁屏. */
static uint8_t recovery_settings(void)
{
    uint8_t sel = 0, k;

    if (!rcv_set) {
        screen_clear();
        put_str(3, 1, "RECOVERY CODE");
        draw_frame(0, 2, 20, 12);
        put_str(4, 4, "NO CODE SET");
        put_str(1, 6, "CREATE 12-DIGIT");
        put_str(1, 7, "RECOVERY CODE?");
        put_str(2, 9, "A=YES  B=NO");
        put_str(2, 16, "B=BACK (HOLD=LOCK)");
        k = ui_getkey_noninput(J_A | J_B);
        if (!k) return 1;
        if (k == KEY_LOCKED) return 1;
        if (k & J_A) {
            if (recovery_create_ui()) return 1;
        }
        return 0;
    }

    while (1) {
        if (!rcv_set) return 0;
        screen_clear();
        put_str(3, 1, "RECOVERY CODE");
        draw_frame(0, 2, 20, 12);
        put_fixed(2, 4, "REPLACE CODE", 12, (uint8_t)(sel == 0));
        put_fixed(2, 6, "REMOVE CODE", 11, (uint8_t)(sel == 1));
        put_str(3, 9, "CODE IS ACTIVE");
        put_str(2, 11, "A=SELECT B=BACK");
        put_str(2, 16, "B=BACK (HOLD=LOCK)");

        k = ui_getkey_noninput(J_UP | J_DOWN | J_A | J_B);
        if (!k) return 1;
        if (k == KEY_LOCKED) return 1;
        if (k & J_B) return 0;
        if (k & J_UP)   { sel = 0; continue; }
        if (k & J_DOWN) { sel = 1; continue; }

        if (k & J_A) {
            if (sel == 0) {
                if (recovery_create_ui()) return 1;
            } else {
                screen_clear();
                draw_frame(0, 2, 20, 12);
                put_str(2, 4, "REMOVE RECOVERY");
                put_str(4, 5, "CODE?");
                put_str(1, 7, "YOU CAN STILL USE");
                put_str(1, 8, "YOUR PIN");
                put_str(2, 10, "A=YES  B=NO");
                k = ui_getkey_noninput(J_A | J_B);
                if (!k) return 1;
                if (k == KEY_LOCKED) return 1;
                if (k & J_A) {
                    pm5_rcv_clear();
                    toast("RECOVERY REMOVED");
                }
            }
        }
    }
}

static uint8_t settings_screen(void)
{
    uint8_t sel = 0, k;
    char v[8];

    while (1) {
        screen_clear();
        put_str(1, 1, "SETTINGS");
        draw_frame(0, 2, 20, 13);
        put_fixed(1, 4, "PASSWORD LENGTH", 15, (uint8_t)(sel == 0));
        put_fixed(1, 6, "SYMBOLS", 7, (uint8_t)(sel == 1));
        put_fixed(1, 8, "AUTO-LOCK", 9, (uint8_t)(sel == 2));
        put_fixed(1, 10, "RECOVERY CODE", 13, (uint8_t)(sel == 3));

        v[0] = (char)('0' + GEN_LENS[gen_idx] / 10);
        v[1] = (char)('0' + GEN_LENS[gen_idx] % 10);
        v[2] = 0;
        put_str(17, 4, v);
        put_str(16, 6, gen_sym ? "ON" : "OFF");
        put_str(16, 10, rcv_set ? "ON" : "OFF");
        if (LOCK_MINS[lock_idx] >= 10) {
            put_str(13, 8, "10 MIN");
        } else {
            v[0] = (char)('0' + LOCK_MINS[lock_idx]);
            v[1] = ' '; v[2] = 'M'; v[3] = 'I'; v[4] = 'N'; v[5] = 0;
            put_str(14, 8, v);
        }

        put_str(1, 16, "A/<> = CHANGE");
        put_str(1, 17, "B=BACK (HOLD=LOCK)");

        k = ui_getkey_noninput(J_UP | J_DOWN | J_LEFT | J_RIGHT | J_A | J_B);
        if (!k) return 1;
        if (k == KEY_LOCKED) return 1;
        if (k & J_B) return 0;
        if (k & J_UP)   { if (sel) sel--; continue; }
        if (k & J_DOWN) { if (sel < 3) sel++; continue; }

        if (k & J_A) {
            if (sel == 3) {
                if (recovery_settings()) return 1;
                continue;
            }
            if (sel == 0) gen_idx = (uint8_t)((gen_idx + 1) & 3);
            else if (sel == 1) gen_sym = (uint8_t)(!gen_sym);
            else lock_idx = (uint8_t)((lock_idx + 1) % 5);
            settings_save();
            if (sel == 2) idle_reset();
        } else if (k & J_RIGHT) {
            if (sel == 0) gen_idx = (uint8_t)((gen_idx + 1) & 3);
            else if (sel == 1) gen_sym = (uint8_t)(!gen_sym);
            else if (sel == 2) lock_idx = (uint8_t)((lock_idx + 1) % 5);
            if (sel < 3) {
                settings_save();
                if (sel == 2) idle_reset();
            }
        } else if (k & J_LEFT) {
            if (sel == 0) gen_idx = (uint8_t)((gen_idx + 3) & 3);
            else if (sel == 1) gen_sym = (uint8_t)(!gen_sym);
            else if (sel == 2) lock_idx = (uint8_t)((lock_idx + 4) % 5);
            if (sel < 3) {
                settings_save();
                if (sel == 2) idle_reset();
            }
        }
    }
}

static uint8_t menu_screen(void)
{
    static const char *items[6] = {
        "VIEW ACCOUNTS", "ADD ACCOUNT", "CHANGE PIN", "LOCK VAULT",
        "RECYCLE BIN", "SETTINGS"
    };
    uint8_t sel = 0, k, i;

    while (1) {
        screen_clear();
        put_str(5, 1, "VAULT MENU");
        draw_frame(0, 2, 20, 14);
        for (i = 0; i < 6; i++) {
            uint8_t y = (uint8_t)(3 + i * 2);
            put_tile(1, y, (uint8_t)(248 + i));
            if (i == sel) {
                put_fixed(2, y, "", 17, 1);
                put_fixed(3, y, items[i], 15, 1);
            } else {
                put_str(3, y, items[i]);
            }
        }
        if (vault_bad_count) {
            char bad[4];
            fmt3_zp(bad, vault_bad_count);
            put_str(1, 16, "BAD ");
            put_str(5, 16, bad);
            put_str(9, 16, "RECORDS");
        } else {
            put_str(1, 16, "HOLD B=QUICK LOCK");
        }
        put_str(1, 17, "PRESS A TO OPEN");
        k = ui_getkey_noninput(J_UP | J_DOWN | J_A | J_B);
        if (!k) return 1;
        if (k == KEY_LOCKED) return 1;
        if (k & J_UP)   { if (sel) sel--; continue; }
        if (k & J_DOWN) { if (sel < 5) sel++; continue; }
        if (k & J_A) {
            if      (sel == 0) { if (list_screen()) return 1; }
            else if (sel == 1) { if (add_flow(-1)) return 1; }
            else if (sel == 2) { if (change_pin()) return 1; }
            else if (sel == 3) return 0;
            else if (sel == 4) { if (bin_screen()) return 1; }
            else            { if (settings_screen()) return 1; }
        }
    }
}

/* ------------------------------------------------------------------ */
/* 入口                                                                */
/* ------------------------------------------------------------------ */

void main(void)
{
    uint16_t i, j, src, dst;

    DISPLAY_OFF;
    set_bkg_data(0, 96, font_tiles);
    /* 反白字形: 96..127 在 0x9600, 128..191 在 0x8800 (LCDC.4=0 有符号寻址).
     * 黑底(颜色1) + 最浅字(颜色0), 供菜单/PIN 选中态使用. */
    for (i = 0; i < 96; i++) {
        src = (uint16_t)(i * 16);
        dst = (uint16_t)(((i < 32) ? 0x9600 : 0x8600) + src);
        for (j = 0; j < 8; j++) {
            ((volatile uint8_t *)dst)[j * 2] = (uint8_t)(font_tiles[src + j * 2] ^ 0xFF);
            ((volatile uint8_t *)dst)[j * 2 + 1] = 0x00;
        }
    }
    /* PIN 图形键帽/边框与开机生成的派生字模 */
    set_bkg_data(192, 9, FRAME_TILES);
    gen_ui_tiles();
    *(volatile uint8_t *)0xFF47 = 0x9C;      /* BGP: 0=最浅 1=黑 2=中绿 3=暗绿 */
    screen_clear();
    SHOW_BKG;
    DISPLAY_ON;

    sram_hw_init();                          /* MBC1 RAM banking 模式 */
    wal_recover();                           /* 先完成上次未提交/已提交的日志 */
    rng_boot();

    screen_clear();
    draw_frame(2, 1, 16, 14);
    put_tile(9, 2, 254);
    put_str(4, 3, "GB PASSWORD");
    put_str(7, 5, "VAULT");
    put_str(6, 8, "V0.3.7.1");
    put_str(4, 11, "PRESS START");
    put_str(1, 16, "BATTERY CART NEEDED");
    getkey(J_START);

    while (1) {
        uint8_t load_state = vault_load();

        if (load_state == LOAD_LEGACY) {
            if (migrate_v03()) {
                idle_start();
                menu_screen();          /* 刚设置过新 PIN, 直接进菜单 */
                idle_stop();
            }
            continue;
        }

        if (load_state == LOAD_PM4) {
            if (!migrate_v04()) recovery_screen(LOAD_CORRUPT);
            continue;
        }

        if (load_state == LOAD_DAMAGED) {
            if (recovery_screen(LOAD_DAMAGED) == 2) damage_ack = 1;
            continue;
        }

        if (load_state == LOAD_CORRUPT || load_state == LOAD_UNSUPPORTED) {
            recovery_screen(load_state);
            continue;
        }

        if (!pin_set) {
            setup_pin();
            idle_start();
            menu_screen();
            idle_stop();
        } else {
            if (unlock()) {
                idle_start();
                menu_screen();
                idle_stop();
            }
        }
    }
}
