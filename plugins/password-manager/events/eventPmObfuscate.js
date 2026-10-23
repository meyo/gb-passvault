/**
 * PM: XOR Mix（异或混淆 / 还原）
 * ------------------------------------------------------------------
 * 把 Value 与 Key 做异或，结果写回 Value。异或是对合的：同一个事件执行两次
 * 就还原成原值，所以「加密」和「解密」都用这一个事件即可。
 *
 * 编译后生成的 GBVM（示例）：
 *   VM_RPN
 *     .R_REF VAR_DB_SECRET_1
 *     .R_REF VAR_SCRAMBLE
 *     .R_OPERATOR .B_XOR
 *     .R_REF_SET VAR_DB_SECRET_1
 *     .R_STOP
 *
 * 用法建议：
 *   - 写入密码后对每个字符执行一次 → 存档里不再是明文；
 *   - 显示前再对每个字符执行一次 → 还原明文。
 *   - 纯可视化替代方案（无插件）：写入时 +7、显示时 -7（见 docs/02）。
 *   - 注意：这只是混淆，不是加密，无法抵御真正的攻击者。
 */

export const id = "PM_XOR_MIX";
export const name = "PM: XOR Mix（异或混淆）";
export const groups = ["Password Manager"];

export const fields = [
  {
    key: "value",
    label: "数据变量 Value",
    description: "要混淆/还原的变量（结果写回该变量）。",
    type: "variable",
    defaultValue: "LAST_VARIABLE",
  },
  {
    key: "key",
    label: "密钥变量 Key",
    description: "存放异或密钥的变量（例：$SCRAMBLE，初始化为某个常量，如 165）。",
    type: "variable",
    defaultValue: "LAST_VARIABLE",
  },
];

export const compile = (input, helpers) => {
  const { getVariableAlias, appendRaw } = helpers;
  const value = getVariableAlias(input.value);
  const key = getVariableAlias(input.key);

  appendRaw(`; === PM XOR Mix: ${value} = ${value} ^ ${key} ===
VM_RPN
  .R_REF ${value}
  .R_REF ${key}
  .R_OPERATOR .B_XOR
  .R_REF_SET ${value}
  .R_STOP`);
};
