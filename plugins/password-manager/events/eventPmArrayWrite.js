/**
 * PM: Array Write（数组写入）
 * ------------------------------------------------------------------
 * 用 GBVM 的间接寻址把 Value 写进「以 Array Base 开头的一段连续变量」中的第 Index 个元素。
 *
 * 伪代码：  script_memory[ArrayBase + Index] = Value
 *
 * 编译后生成的 GBVM（示例）：
 *   VM_RPN
 *     .R_INT16 VAR_DB_LABEL        ; 压入数组首地址（汇编期常量 = 变量编号）
 *     .R_REF VAR_TMP_INDEX         ; 压入运行期下标
 *     .R_OPERATOR .ADD
 *     .R_REF_SET VAR_TMP_SCRATCH   ; scratch = base + index
 *     .R_STOP
 *   VM_SET_INDIRECT VAR_TMP_SCRATCH, VAR_VALUE
 *   ; VM_SET_INDIRECT 的语义（gbvm 源码 vm.c）: script_memory[scratch] = value
 *
 * 注意：
 *  - 四个字段都必须选「全局变量」，不要传自定义脚本参数（ByRef 参数是栈上局部变量）。
 *  - scratch 必须是「另一个」不会被同时使用的变量。
 *  - Array Base 之后的元素必须在变量表中连续编号（重命名保持默认顺序即可）。
 */

export const id = "PM_ARRAY_WRITE";
export const name = "PM: Array Write（数组写入）";
export const groups = ["Password Manager"];

export const fields = [
  {
    key: "arrayBase",
    label: "数组首变量 Array Base",
    description:
      "连续变量段的第一个变量（例：$DB_LABEL）。运行时会写入 基址+Index 对应的那个变量。",
    type: "variable",
    defaultValue: "LAST_VARIABLE",
  },
  {
    key: "index",
    label: "元素序号 Index",
    description: "0 起算的数组下标，存于变量中，可为运行期动态值。",
    type: "variable",
    defaultValue: "LAST_VARIABLE",
  },
  {
    key: "value",
    label: "写入值 Value",
    description: "要写入该元素的值（通常是要存储的字符编码）。",
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
  const value = getVariableAlias(input.value);
  const scratch = getVariableAlias(input.scratch);

  appendRaw(`; === PM Array Write: script_memory[${base} + ${index}] = ${value} ===
VM_RPN
  .R_INT16 ${base}
  .R_REF ${index}
  .R_OPERATOR .ADD
  .R_REF_SET ${scratch}
  .R_STOP
VM_SET_INDIRECT ${scratch}, ${value}`);
};
