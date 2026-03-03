# Intrinsic 聚合类型支持设计说明（Struct 基线 + Array/MemRef 最终方案）

## 1. 背景与目标

本文档总结两阶段能力演进：

1. **基线阶段（Struct）**：为 LLVM intrinsic 类型系统增加 `struct` 表达能力，并打通 GlobalISel 对 intrinsic 聚合参数/返回的匹配。  
2. **最终阶段（Array + MemRef）**：在上述能力上扩展 `array`，并提供可复用的 memref 描述符建模与 rank 扩展宏，以支持 MLIR lowered memref descriptor 场景。

核心目标是：

- 在 intrinsic 签名中直接表达聚合类型（struct / array）。
- 在 TableGen / GlobalISel / DAG pattern 侧按“**flatten 后的叶子元素**”进行匹配与约束。
- 让前端或方言侧以更少重复定义，声明 rank 变化的 memref intrinsic 族。

---

## 2. 基线：Struct 支持（已在基线分支）

### 2.1 类型表达层

基线在 `Intrinsics.td` 中引入了 `LLVMStructType<...>`，用于在 intrinsic 参数/返回里表达聚合结构。其编码沿用 IIT 体系：

- 空 struct 使用 `IIT_EMPTYSTRUCT`。
- 非空 struct 使用 `IIT_STRUCT` + 元素个数编码 + 递归元素签名。

### 2.2 GlobalISel 导入语义

基线还修复了 intrinsic 翻译/匹配中“聚合参数只允许单 vreg”的限制，使聚合展开后可作为多个 operand/use 进入匹配流程。

### 2.3 TableGen 语义

在 TableGen 侧，基线引入“flatten 视图”概念：

- 返回值个数、参数个数不再只看顶层 `RetTys/ParamTys`，而看 flatten 后叶子数。
- ImmArg/Pointer 判定也要映射到 flatten 后参数索引。

这一步是后续 array/memref 方案成立的前置条件。

---

## 3. 最终方案：Array + MemRef

### 3.1 Array 一等公民化

#### 3.1.1 IIT 与 IR 匹配层

新增 `IIT_ARRAY` / `IITDescriptor::Array`，并在 IR intrinsic 解码与匹配中完整支持：

- `DecodeIITType`：解析数组长度与元素类型描述。
- `DecodeFixedType`：还原为 LLVM `ArrayType`。
- `matchIntrinsicType`：按长度 + 元素类型递归匹配。

这使 array 在 intrinsic 签名中不再是“外部约定”，而是可被 LLVM intrinsic 基础设施理解的类型。

#### 3.1.2 TableGen flatten

`CodeGenIntrinsics` flatten 逻辑扩展为：

- struct：递归展开元素。
- array：按元素数量重复展开元素类型（元素本身可继续递归）。

因此 `array<2 x i32>` 将被视为两个 `i32` 叶子 operand。

### 3.2 MemRef 描述符建模

在 `Intrinsics.td` 提供固定布局的 `LLVMMemRefDescriptorType<rank>`：

```text
{ ptr allocatedPtr,
  ptr alignedPtr,
  i64 offset,
  [rank x i64] sizes,
  [rank x i64] strides }
```

并提供 rank 固定别名：

- `llvm_rank1_memref_ty` ... `llvm_rank5_memref_ty`。

### 3.3 单点声明、多 rank 展开

提供 marker + multiclass 机制：

- marker：`llvm_memref_ty`。
- helper：`LLVMMemRefIntrinsic`。

定义时可写：

```tablegen
let TargetPrefix = "mytarget" in {
  defm int_mytarget_memref_add : LLVMMemRefIntrinsic<
      [], [llvm_memref_ty]>;
}
```

自动生成：

- `int_mytarget_memref_add_rank1` ... `rank5`。

内部通过 `ReplaceMemRefMarker` 将 `llvm_memref_ty` 替换为对应 `llvm_rankN_memref_ty`。

### 3.4 放置位置与组织

为可维护性，memref 相关基础类型定义放在 `llvm_*_ty` 基础类型定义之后；
`LLVMMemRefIntrinsic` 作为 `Intrinsic` 派生辅助，放在 `Intrinsic/DefaultAttrsIntrinsic` 定义之后。

---

## 4. 关键设计权衡

### 4.1 为什么按 flatten 叶子匹配？

优点：

- 与 MachineIR/GlobalISel 实际 operand 形态一致。
- 匹配器与约束逻辑统一，不需要“聚合整体”特例。
- 便于 ImmArg/Pointer 等属性复用既有机制。

代价：

- 语义上“一个参数”在 matcher 里会变成多个叶子参数。
- 测试 pattern 必须使用正确叶子类型（例如 memref 前两项是 `ptr`，其余是 `i64`）。

### 4.2 为什么 memref helper 固定 `ptr + i64`？

优点：

- 与当前 MLIR lowered memref descriptor 约定一致。
- 避免模板参数化带来的类型组合膨胀与测试复杂度。
- 用户心智更稳定：marker 一替换就是标准 descriptor。

代价：

- 若未来需要非 `i64` index 或特定 pointer 变体，需要新增专门 helper（而非重用同一 helper 参数化）。

---

## 5. 与基线方案的关系

- **基线 struct 支持**提供了聚合 flatten 的总体框架。  
- **最终 array/memref 方案**是在同一 flatten 架构上的自然扩展：
  - struct + array 统一递归展开；
  - DAG/GlobalISel 统一按 flatten 视图做检查与匹配；
  - memref 只是“由 struct+array 组成的具体高频模型”，并通过 marker/multiclass 提供声明层抽象。

这保证了方案一致性：不是“为 memref 单独打补丁”，而是“在聚合类型体系上增量建模”。

---

## 6. 方案收益（相对基线/历史）

1. **表达能力增强**：intrinsic 可直接描述 array 与 memref descriptor。  
2. **匹配行为可预测**：统一 flatten 语义，减少“顶层参数个数”与“实际 matcher 个数”不一致问题。  
3. **声明成本下降**：`LLVMMemRefIntrinsic` 支持单点声明自动展开 rank1..5 变体。  
4. **实现可维护**：IIT 解码、类型匹配、TableGen flatten、DAG 约束四层逻辑保持一致演进。  
5. **对 GlobalISel 友好**：聚合在导入阶段即可落为叶子 operand，避免后续 pass 做额外聚合拆解。

---

## 7. 后续可演进方向

- 将 rank 扩展范围从固定 1..5 提升为可配置列表（例如由 `defset` 驱动）。
- 在保持固定布局 helper 的同时，增加“可选扩展 helper”满足特定后端 index/pointer 变体需求。
- 补充更多 lit 覆盖：
  - 多 marker 参数（一个 intrinsic 带多个 memref 参数）；
  - `arg_prefix/arg_suffix` 与 marker 混用；
  - 返回值含 struct/array/memref 组合场景。

