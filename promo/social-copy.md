# GB Password Vault — 社交平台宣传文案

适用版本：**V0.3.7.4**  
仓库：<https://github.com/meyo/gb-passvault>  
下载：<https://github.com/meyo/gb-passvault/releases/latest>

> 名称说明：仓库名、宣传图、三份 README、插件说明和本文案已统一使用 **GB Password Vault**；界面标题画面原本就是 `GB PASSWORD VAULT`。卡带内部标题仍为 `PASSMGR`，这是 6 字符限制下的烧录/存档兼容名称。

---

## 1. 图片与用途

| 文件 | 尺寸 | 建议用途 |
|---|---|---|
| `gb-passvault-hero-1600x900.jpg` | 1600×900 | X 主帖、Reddit 第二张图、链接预览 |
| `gb-passvault-screens-1600x900.png` | 1600×900 | Reddit 主图、X 回复、README 展示 |
| `gb-passvault-square-1200x1200.jpg` | 1200×1200 | Instagram / Threads / Mastodon，X 也可用 |
| `gb-passvault-github-1280x640.jpg` | 1280×640 | GitHub 仓库 Settings → Social preview |

发布时优先保留原始尺寸，不要二次压缩。

---

## 2. X 文案

### 中文

```text
把密码本装进一台 Game Boy。🎮

GB Password Vault：离线 / 开源 / 无云同步
255 槽 · 6 位 PIN · 可选恢复码 · PMG5 CRC16+WAL
实机 DMG 已验证。

业余项目，未做安全审计，SRAM 未加密。
https://github.com/meyo/gb-passvault
#GameBoy #开源
```

### English

```text
A password vault that runs on a real Game Boy. 🎮

255 slots · 6-digit PIN · optional recovery code
Offline. Open source. No cloud.
PMG5 CRC16 + WAL save protection.

Hobby project, no security audit. SRAM is not encrypted — keep backups.
https://github.com/meyo/gb-passvault
#GameBoy
```

### 日本語

```text
Game Boyで動くパスワード帳。🎮

GB Password Vault：オフライン／オープンソース
255枠・6桁PIN・任意の回復コード・PMG5 CRC16+WAL
実機DMGで動作確認済み。

趣味プロジェクトで、監査未実施・SRAMは非暗号化です。
https://github.com/meyo/gb-passvault
#GameBoy #オープンソース
```

**发布方式**

- 主帖配 `hero`；把 `screens` 作为回复（或第二张图）发出，避免主帖信息过密。
- 链接放在正文末尾；X 会把链接统一按 23 字符计算。
- 如果想更短，可删掉标签行，只保留 `#GameBoy`。

---

## 3. Reddit 文案

### English — r/Gameboy 等

**Title（二选一）**

```text
[OC] I built an offline password vault for the original Game Boy
```

```text
A password manager that runs on a real Game Boy (open source, GBDK-2020)
```

**Body**

```text
I built a small offline password vault that runs on real Game Boy hardware (DMG / GBC / GBA).

- 255 slots · 20-char labels/usernames · 36-char passwords
- 6-digit PIN + optional 12-digit recovery code
- 32KB battery-backed SRAM
- PMG5 CRC16 + WAL save protection, 16-entry recycle bin
- Random password generator, auto-lock
- GBDK-2020, open source, 357/357 tests, verified on a real DMG

Hobby project, no security audit: the PIN locks the UI, but SRAM is not encrypted and can be read. Keep backups.

Repo: https://github.com/meyo/gb-passvault

Not affiliated with Nintendo.
```

### 中文

**标题**

```text
[OC] 我把离线密码本做进了真实 Game Boy
```

**正文**

```text
做了一个能在真实 Game Boy（DMG / GBC / GBA）上运行的离线密码本。

• 255 个槽位：标签/用户名 20 字符，密码 36 字符
• 6 位 PIN + 可选 12 位恢复码
• 32KB 电池存档
• PMG5 双头 + CRC16 + WAL 存档保护，16 条回收站
• 随机密码生成、自动锁屏
• GBDK-2020 编写，357/357 自动化测试，实机 DMG 已验证

这是业余项目，未做安全审计：PIN 只是界面锁，SRAM 未加密、内容可被读取。请保留备份。

仓库：https://github.com/meyo/gb-passvault

与任天堂无关。
```

### 日本語

**タイトル**

```text
[OC] 実機Game Boyで動くオフラインのパスワード帳を作りました
```

**本文**

```text
実機のGame Boy（DMG / GBC / GBA）で動く、オフラインのパスワード帳です。

・255スロット：ラベル/ユーザー名20文字、パスワード36文字
・6桁PIN + 任意の12桁回復コード
・32KB電池バックアップSRAM
・PMG5デュアルヘッダ + CRC16 + WAL、16件のゴミ箱
・ランダムパスワード生成、自動ロック
・GBDK-2020、357/357テスト、実機DMGで確認済み

趣味のプロジェクトで、セキュリティ監査は受けていません。PINは画面ロックで、SRAMは暗号化されておらず読めます。バックアップを保管してください。

リポジトリ：https://github.com/meyo/gb-passvault

任天堂とは関係ありません。
```

**发布方式**

- 用图片帖：第一张放 `screens`，第二张放 `hero`；链接放在正文或第一条评论里，降低被自动删除的概率。
- 标题保留 `[OC]`；发帖前先看目标 subreddit 的 self-promotion 规则。
- 先别同时发很多个 subreddit；挑 1–2 个最相关的，回复评论比多帖更有效。

---

## 4. Alt text（上传图片时填写）

### Hero 1600×900

- **中文**：实机 Game Boy DMG 正在显示 GB Password Vault 的 VAULT MENU；右侧文字列出 255 个密码槽、6 位 PIN、PMG5 CRC16 + WAL 存档保护、32KB SRAM 与开源信息。
- **English**: A real Game Boy DMG running the GB Password Vault menu, with text listing 255 password slots, a 6-digit PIN, PMG5 CRC16 + WAL save protection, 32KB SRAM, and open source.
- **日本語**: 実機Game Boy DMGにGB Password VaultのVAULT MENUが表示されている写真。右側に255スロット、6桁PIN、PMG5 CRC16+WAL、32KB SRAM、オープンソースの記載。

### Screens 1600×900

- **中文**：六张 Game Boy 截图：主菜单、账号列表、账号详情、设置、回收站、存档损坏提示。
- **English**: Six Game Boy screenshots: vault menu, account list, account detail, settings, recycle bin, and a save-damaged warning.
- **日本語**: Game Boyの画面6枚：VAULT MENU、アカウント一覧、詳細、設定、ゴミ箱、セーブ破損の警告。

### Square 1200×1200

- **中文**：GB Password Vault 方形宣传卡，实机 Game Boy 特写，标题为 GB PASSWORD VAULT，标签为 6-DIGIT PIN、PMG5 SAVE、OPEN SOURCE。
- **English**: Square GB Password Vault card with a close-up of the Game Boy, headline GB PASSWORD VAULT, and tags 6-DIGIT PIN, PMG5 SAVE, OPEN SOURCE.
- **日本語**: GB Password Vaultの正方形カード。Game Boyの接写、GB PASSWORD VAULTの見出し、6-DIGIT PIN、PMG5 SAVE、OPEN SOURCEのタグ。

### GitHub 1280×640

- **中文**：GB Password Vault 的 GitHub 社交预览图，包含实机照片、项目名和主要特性。
- **English**: GitHub social preview for GB Password Vault with a hardware photo, project name, and key features.
- **日本語**: GB Password VaultのGitHubソーシャルプレビュー。実機写真、プロジェクト名、主な特徴を掲載。

---

## 5. 一句话短版 / 标签

适合引用帖、个人简介、评论区：

- **中文**：在 1989 年的 Game Boy 上管理 255 个密码。离线、开源、带电池存档。
- **English**: 255 passwords. One Game Boy. Offline, open source, battery-backed.
- **日本語**: 255個のパスワードを、Game Boyで。オフライン、オープンソース、電池バックアップ。

推荐标签：

```text
#GameBoy #GBDK #OpenSource #RetroGaming #PasswordManager
```

中文可用 `#GameBoy #开源 #复古游戏`，日文可用 `#GameBoy #オープンソース #レトロゲーム`。每条控制在 2–3 个即可。

---

## 6. 诚实声明与用词边界

可重复使用的声明：

> Hobby project with no security audit. The PIN locks the UI; SRAM is not encrypted and can be read. Keep backups and don't use it as the only copy of critical passwords. Not affiliated with Nintendo.

> 业余项目，未做安全审计。PIN 只是界面锁，SRAM 未加密、内容可被读取。请保留备份，不要作为重要密码的唯一副本。与任天堂无关。

> 趣味プロジェクトで、セキュリティ監査は受けていません。PINは画面ロックで、SRAMは暗号化されておらず読めます。バックアップを保管し、重要なパスワードの唯一の保管場所にしないでください。任天堂とは関係ありません。

**建议避免**

- ❌ encrypted / 加密 / 暗号化
- ❌ secure / 安全 / 安全
- ❌ military-grade / 军用级
- ❌ audited / 已审计
- ❌ “可以替代 1Password / Bitwarden”

**建议使用**

- ✅ offline / 离线 / オフライン
- ✅ PIN-locked UI / PIN 界面锁 / PINロック
- ✅ save integrity / 存档完整性 / セーブ保護
- ✅ hobby project / 业余项目 / 趣味プロジェクト

---

## 7. 发布前检查清单

- [x] 统一项目名：GB Password Vault（README 标题、插件说明、宣传图、本文案）
- [ ] 确认 GitHub Release `v0.3.7.4` 已上传 `passmgr.gb` 与 `passmgr.gb.sha256`
- [ ] GitHub 仓库 Settings → Social preview 上传 `gb-passvault-github-1280x640.jpg`
- [ ] X 主帖配 `hero`，回复配 `screens`
- [ ] Reddit 图片帖先放 `screens`，正文放仓库链接
- [ ] 上传时填写对应语言的 alt text
- [ ] 检查目标 subreddit 的 self-promotion / OC 规则
- [ ] 确认文案中没有“加密”“安全”“可替代密码管理器”等过度承诺
