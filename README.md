# GB 密码本（GB Password Keeper）

> ⚠️ **项目已转向实机 ROM 路线（GBDK-2020 C 直编）**：交付物 `build/passmgr.gb`（**V0.3.6.1**，
> 255 槽 + 20 字符标签/用户名 + 6 位 PIN + 随机密码生成器 + 32KB SRAM 存档 +
> 回收站/自动锁屏/生成器设置 + **2bpp 图形 PIN 界面** + **损坏存档保护/旧 PIN 验证/快速锁定**），
> **255/255 自动化测试通过**。**当前工程现状请看
> [`docs/05-GBDK实机版实现总结.md`](docs/05-GBDK实机版实现总结.md)**。
> 本 README 与 `docs/01~04`、`plugins/`、`assets/` 为早期 GB Studio 方案资料，仅作设计参考。
> V0.3.3 的 ASCII `+--+` 分格 PIN 界面已作废；V0.3.4 按 `art/gb_login_filled.png` 重做为
> 2bpp 图形分格框 + 立体键帽。V0.3.5 将 6 个分格框从 2×3 加宽为 **3×3 tile**。
> V0.3.6 增加 9-slice 面板/图标/页眉页脚、菜单与列表选中条、详情字段卡、回收站空状态；
> PIN 画面保持 V0.3.5 外观。
> **V0.3.6.1 修正**：详情页改为完整封闭边框（左右边框不再缺失）；快速锁定从
> `B+SELECT` 改为在非输入画面**长按 B 约 1.5 秒**，短按 B 仍是返回。
>
> **V0.3.6.1 快速上手**：烧录 `build/passmgr.gb`（MBC1+RAM+BATTERY，需 ≥32KB SRAM 存档的卡带），
> 首次开机设 **6 位 PIN**。PIN 画面采用 `SECRET VAULT` 标题 + 6 个加宽图形分格框 + 立体数字键盘
> （`CLR` 清空全部，`B` 退格，`START` 确认）。主菜单 `ADD ACCOUNT` 依次输入标签（≤20）、用户名（≤20，可留空）、
> 密码（≤36；密码页按 `SELECT` 生成随机强密码，再按重掷）。
> `VIEW ACCOUNTS` 列表：上下逐条（按住连滚）、**左右整页翻 12 条**、
> **`SELECT` 切换标签/用户名视图**；选中行超长标签自动跑马灯。
> 详情页按住 `A` 看明文，**同时按 `SELECT+START` 编辑**，单按 `START` 删除（**进回收站**，可恢复）。
> `SETTINGS` 可设生成密码长度（12/16/24/32）、符号开关、**自动锁屏时长（1/2/3/5/10 分钟）**；
> `RECYCLE BIN` 保存最近删除的 16 条记录：`A` 恢复、`SELECT` 永久删除单条、`START` 清空全部（均有确认）。
> **快速锁定**：在菜单/列表/详情/设置/回收站**长按 B 约 1.5 秒**，立即回到 PIN 画面；
> 短按 B 仍是返回。文本输入和 PIN 输入画面不启用（避免与 `B=删除` 冲突）。
>
> **损坏存档保护**：PMG4 校验失败时显示 `SAVE CORRUPT`，未知格式显示 `UNSUPPORTED SAVE`，
> **不会自动清空**。可只读扫描可识别记录，`A=RETRY` 重试，`START` 后再按 `A` 才显式擦除。
>
> **从 V0.3 升级**：直接保留电池存档，上电后按提示输入旧 **4 位 PIN**，再设置新 **6 位 PIN**；
> 旧账号的标签和密码会完整迁移，用户名留空，可逐条补填。迁移画面出现 `DO NOT POWER OFF`
> 时请勿断电。迁移后旧 V0.3 ROM 无法读取新存档（V0.3 基线保留在 `archive/v0.3/`）。
> **从 V0.3.1–V0.3.6 升级**：PMG4 存档直接沿用，无需迁移。
> **升级前建议先备份（dump）存档。**

## 首次克隆 / 构建

仓库不提交解压后的 `tools/gbdk/` 和 `tools/emu/node_modules/`，但附带固定的 `tools/gbdk-macos.tar.gz`。首次克隆后运行：

```bash
./tools/bootstrap.sh
./build.sh
node tools/emu/smoke.js
```

`bootstrap.sh` 会在缺少工具链时解压 GBDK 并安装 `serverboy`；之后 `build.sh` 生成 `build/passmgr.gb`，冒烟测试结果写入 `test_out/smoke.log`。

> 以下为早期 **GB Studio 4.x** 原型方案的资料（历史归档，不是当前 ROM 的实现）：
>
> 一个用 **GB Studio 4.x** 制作、跑在 **Game Boy / GBC 实机或模拟器**上的
> 复古「离线密码本」项目框架 + 核心逻辑脚本。

- 🕹 主菜单：查看账号 / 编辑槽位 / 修改 PIN / 锁定
- 🗄 数据模型：8 个账号槽 × 12 字符标签 + 12 字符密码（字符编码数组）
- 💾 持久化：`Game Data Save/Load`（SRAM + 电池备份），关机不丢
- ⌨️ 虚拟键盘：48 键网格 + 方向键光标 + A 键确认（另有 4 位数字 PIN 小键盘）
- 🔒 解锁 PIN：4 位数字、+7 混淆保存、输错 3 次触发彩蛋
- 🧩 可选插件：把"数组读写/异或混淆"变成三条自定义事件（GBVM 间接寻址）

> ⚠️ 诚实声明：这是玩具级的"仪式感"密码本，SRAM 内容可被读取，
> 请不要存真实的重要密码（详见 `docs/04` 第 8 节）。

---

## 目录结构

```
gbs-password-manager/
├── README.md                       ← 你在这里
├── docs/
│   ├── 01-架构与场景规划.md          ← 6 个场景、跳转图、背景画法
│   ├── 02-核心事件逻辑.md            ← 每个场景的 GB Studio 事件配方 + 伪代码
│   ├── 03-变量表与数据模型.md        ← 512 变量怎么分工、连续段约定
│   └── 04-开发注意事项与坑.md        ← 硬件/引擎/文本/存档各种坑
├── plugins/
│   └── password-manager/
│       ├── README.md               ← 插件安装与使用说明
│       └── events/
│           ├── eventPmArrayWrite.js    ← 数组写入（间接寻址）
│           ├── eventPmArrayRead.js     ← 数组读取
│           └── eventPmObfuscate.js     ← 异或混淆/还原
└── assets/
    └── keymap-table.csv            ← 48 键键位表（喂给 Data Table Lookup）
```

## 快速开始

1. GB Studio 4.3+ 新建项目 → 卡带类型选 **带 BATTERY** 的（Project Settings）；
2. 打开 `docs/03`，按编号顺序把变量重命名好（连续段不要断号）；
3. （可选）把 `plugins/password-manager` 复制进项目根目录的 `plugins/`，
   重启编辑器 → 事件列表出现 **Password Manager** 分组；
4. 按 `docs/01` 建 6 个场景和静态背景，按 `docs/02` 贴事件；
5. 跑 `docs/02` 第 10 节的"通关自测清单"验证。

## 推荐阅读顺序

- 想先理解结构：`01` → `03` → `02`
- 想直接开干：`03`（建变量）→ `02`（贴逻辑）→ `04`（遇到问题diff）
- 想学 GBVM / 插件：`plugins/password-manager/README.md` + `04` 第 7 节

## 版本说明

- 目标：GB Studio **4.3.x（稳定版）**；4.2 及更早可用"纯可视化替代方案"（`02` 第 8 节）；
- 4.4 开发版引入**原生数组变量**后，可直接替换插件用法，数据模型无需改动。
