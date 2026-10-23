#!/bin/sh
# GB Password Vault - 构建脚本 (GBDK-2020)
set -e
cd "$(dirname "$0")"

GBDK=tools/gbdk
OUT=build/passmgr.gb

mkdir -p build

$GBDK/bin/lcc \
    -Wa-l -Wl-m -Wl-j \
    -Wm-yt0x03 \
    -Wm-ya4 \
    -Wm-ynPASSMGR \
    -o "$OUT" \
    src/main.c src/font_data.c

echo "== 构建成功: $OUT =="
ls -la "$OUT"
echo
echo "== 卡带头部自检 =="
python3 - "$OUT" <<'EOF'
import sys
rom = open(sys.argv[1], "rb").read()
title = rom[0x134:0x144].split(b"\x00")[0].decode("ascii", "replace")
print("标题      (0x134):", repr(title))
print("CGB 标志  (0x143): 0x%02X" % rom[0x143])
print("卡带类型  (0x147): 0x%02X" % rom[0x147], "(期望 0x03 = MBC1+RAM+BATTERY)")
print("ROM 大小  (0x148): 0x%02X" % rom[0x148])
print("RAM 大小  (0x149): 0x%02X" % rom[0x149], "(期望 0x03 = 32KB / 4 x 8KB 存储体)")
computed = 0
for i in range(0x134, 0x14D):
    computed = (computed - rom[i] - 1) & 0xFF
print("头部校验和(0x14D): 0x%02X (计算值 0x%02X) %s" % (
    rom[0x14D], computed, "OK" if rom[0x14D] == computed else "*** 错误 ***"))
print("ROM 总大小:", len(rom), "字节")
EOF
