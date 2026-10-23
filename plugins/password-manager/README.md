# password-manager 插件（可选）

为「GB 密码本」项目提供的自定义脚本事件（GB Studio 4.x 脚本事件插件）。

## 安装

1. 把整个 `password-manager` 文件夹复制到你的 GB Studio 项目根目录下的 `plugins/` 文件夹：

   ```
   我的密码本.gbsproj
   plugins/
     password-manager/
       events/
         eventPmArrayWrite.js
         eventPmArrayRead.js
         eventPmObfuscate.js
   ```

2. 关闭并重新打开项目（或重启 GB Studio）。
3. 在任意脚本里点「Add Event」，事件分组里会出现 **Password Manager**。

## 事件一览

| 事件 | 作用 | 生成的 GBVM 核心 |
| --- | --- | --- |
| PM: Array Write（数组写入） | `数组[Index] = Value` | `VM_RPN` 求 `base+index` → `VM_SET_INDIRECT` |
| PM: Array Read（数组读取） | `Dest = 数组[Index]` | `VM_RPN` 求 `base+index` → `VM_GET_INDIRECT` |
| PM: XOR Mix（异或混淆） | `Value = Value ^ Key` | `VM_RPN` + `.B_XOR` |

## 使用须知（重要）

- **只支持全局变量**：四个参数的变量选择里不要传「自定义脚本的参数变量」（ByRef 参数在栈上，地址算法不同）。
- **Scratch 必须独立**：`Scratch` 要选一个在当前脚本执行期间不会被别的逻辑同时使用的变量。
- **数组必须连续**：`Array Base` 表示一段连续编号的变量，后续元素按变量编号 +1、+2……排列。
  在 GB Studio 的变量列表里按顺序重命名即可保证（不要打乱顺序）。
- **4.3 及更早版本**：变量是固定的 512 个编号槽位，编号天然连续；
  **4.4（开发版）**以后变量按需创建，请确保数据库范围内的每个变量都在变量列表里真实存在，
  否则间接寻址可能越界。
- 这些事件依赖 GBVM 的公开操作码 `VM_RPN / .R_INT16 / .R_REF / .R_OPERATOR / .R_REF_SET / .R_STOP /
  VM_SET_INDIRECT / VM_GET_INDIRECT`，行为与 GB Studio 自身生成代码所用模式一致。
  如果某个版本编译报错，打开 Debugger 的编译器输出对照调整即可。

## 不想用插件？

完全可以。`docs/02-核心事件逻辑.md` 里给出了等价的纯可视化事件方案：
用 12 分支的「If Variable Compare With Value」链代替数组写入，
用「Evaluate Math Expression」的 `+7 / -7` 代替异或。
插件只是让脚本更短、更接近"真正的数组"。
