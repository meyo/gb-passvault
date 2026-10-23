/**
 * PM: Array Read（数组读取）
 * ------------------------------------------------------------------
 * 读取「以 Array Base 开头的一段连续变量」中的第 Index 个元素，存入 Dest。
 *
 * 伪代码：  Dest = script_memory[ArrayBase + Index]
 *
 * 编译后生成的 GBVM（示例）：
 *   VM_RPN
 *     .R_INT16 VAR_DB_SECRET
 *     .R_REF VAR_TMP_INDEX
 *     .R_OPERATOR .ADD
 *     .R_REF_SET VAR_TMP_SCRATCH
 *     .R_STOP
 *   VM_GET_INDIRECT VAR_DEST, VAR_TMP_SCRATCH
 *   ; VM_GET_INDIRECT(dest, holder) 的语义: dest = script_memory[holder]
 *   （注意它与 SET 的参数方向不同：SET 用第 1 个操作数做间接地址，GET 用第 2 个）
 */

export const id = "PM_ARRAY_READ";
export const name = "PM: Array Read（数组读取）";
export const groups = ["Password Manager"];

export const fields = [
  {
    key: "arrayBase",
    label: "数组首变量 Array Base",
    description: "连续变量段的第一个变量（例：$DB_SECRET）。",
    type: "variable",
    defaultValue: "LAST_VARIABLE",
  },
  {
    key: "index",
    label: "元素序号 Index",
    description: "0 起算的数组下标，存于变量中。",
    type: "variable",
    defaultValue: "LAST_VARIABLE",
  },
  {
    key: "dest",
    label: "读取到 Dest",
    description: "接收读取结果的目标变量。",
    type: "variable",
    defaultValue: "LAST_VARIABLE",
  },
  {
    key: "scratch",
    label: "临时变量 Scratch",
    description: "存放计算出的目标地址，必须是另一个空闲的全局变量。",
    type: "variable",
    defaultValue: "LAST_VARIABLE",
  },
];

export const compile = (input, helpers) => {
  const { getVariableAlias, appendRaw } = helpers;
  const base = getVariableAlias(input.arrayBase);
  const index = getVariableAlias(input.index);
  const dest = getVariableAlias(input.dest);
  const scratch = getVariableAlias(input.scratch);

  appendRaw(`; === PM Array Read: ${dest} = script_memory[${base} + ${index}] ===
VM_RPN
  .R_INT16 ${base}
  .R_REF ${index}
  .R_OPERATOR .ADD
  .R_REF_SET ${scratch}
  .R_STOP
VM_GET_INDIRECT ${dest}, ${scratch}`);
};
