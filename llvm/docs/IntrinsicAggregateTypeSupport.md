# LLVM Intrinsic 聚合类型支持设计说明

## 1. 背景与目标

本设计为 LLVM intrinsic 类型系统新增聚合类型表达与匹配能力，覆盖：

- `array` 类型描述与 IIT 编解码；
- memref 描述符类型建模（固定布局，按 rank 实例化）；
- TableGen / GlobalISel / DAG pattern 的统一 flatten 语义。

目标是让 intrinsic 签名、类型匹配和指令选择链路对聚合类型具备一致、可预测的行为。

---

## 2. 相对原有 LLVM 的核心变化

### 2.1 Intrinsic IIT 类型系统扩展（Array）

新增 `IIT_ARRAY` 与对应的 `IITDescriptor::Array`，并打通：

- IIT 表解码；
- 固定 LLVM 类型构造（`ArrayType`）；
- intrinsic 调用签名匹配（长度 + 元素类型递归匹配）。

这使 array 从“外部约定”变为 LLVM intrinsic 核心基础设施的一等类型。

### 2.2 TableGen 类型表达扩展

新增 `LLVMArrayType<num_elements, element_type>`，用于直接在 intrinsic 签名中表达固定大小数组。

并新增 `LLVMMemRefDescriptorType<rank>`（固定 `ptr/ptr/i64/[rank x i64]/[rank x i64]` 布局）用于表达 MLIR lowered memref descriptor。

### 2.3 Marker + 宏展开的 memref intrinsic 声明能力

新增：

- marker 类型：`llvm_memref_ty`；
- rank 具体别名：`llvm_rank1_memref_ty ... llvm_rank5_memref_ty`；
- 替换工具：`ReplaceMemRefMarker`；
- 声明宏：`LLVMMemRefIntrinsic`。

开发者可通过单个 `defm` 声明自动生成 rank1..5 的 intrinsic 族，而无需手写 5 份重复定义。

### 2.4 统一 flatten 语义（TableGen/GlobalISel/DAG）

`CodeGenIntrinsics` 中的 flatten 逻辑扩展为同时递归处理 struct 与 array：

- struct：递归展开元素；
- array：按元素个数重复展开元素类型（元素可继续递归）。

`CodeGenDAGPatterns` 的 intrinsic 检查与类型约束改为基于 flatten 后的结果/参数列表，不再依赖顶层计数。

这消除了“顶层签名个数”与“匹配器实际 operand 个数”不一致的问题。

---

## 3. 设计细节

### 3.1 Array 的 IIT/IR 路径

Array 在三处形成闭环：

1. `DecodeIITType`：读取数组长度与元素描述；
2. `DecodeFixedType`：构造 `ArrayType`；
3. `matchIntrinsicType`：按长度和元素递归匹配。

### 3.2 MemRef 描述符模型

`LLVMMemRefDescriptorType<rank>` 固定布局：

```text
{ ptr allocatedPtr,
  ptr alignedPtr,
  i64 offset,
  [rank x i64] sizes,
  [rank x i64] strides }
```

`rank` 为编译期常量，匹配 LLVM intrinsic 静态签名模型。

### 3.3 单声明多 rank 生成

示例：

```tablegen
defm int_memref_elem_add : LLVMMemRefIntrinsic<
    [], [llvm_memref_ty]>;
```

自动生成：

- `int_memref_elem_add_rank1` ... `rank5`。

### 3.4 组织与放置

- memref 相关基础类型别名定义放在 `llvm_*_ty` 基础类型定义之后；
- `LLVMMemRefIntrinsic` 放在 `Intrinsic/DefaultAttrsIntrinsic` 之后，保证派生关系清晰。

---

## 4. 方案优势

1. **表达能力提升**：intrinsic 可原生描述 array 与 memref descriptor。  
2. **行为一致性提升**：IIT 解码、类型匹配、TableGen flatten、DAG 约束使用同一语义。  
3. **匹配可预测**：避免因顶层参数计数导致的 matcher reject/不导入问题。  
4. **声明成本降低**：通过 marker + `defm` 自动展开 rank 变体。  
5. **维护成本降低**：减少手写 rank 变体复制粘贴与分叉逻辑。

---

## 5. 关键权衡

### 5.1 Flatten 叶子匹配

优点：

- 与 MachineIR operand 现实一致；
- 匹配逻辑更统一。

代价：

- 测试/模式需使用正确叶子类型（memref 前两项为 ptr，后续为 i64）。

### 5.2 固定 memref 字段类型（ptr + i64）

优点：

- 与当前 MLIR lowered descriptor 一致；
- 降低模板组合复杂度。

代价：

- 若未来需要变体字段类型，需新增扩展 helper。

---

## 6. 后续建议

- 支持可配置 rank 集（而非固定 1..5）。
- 增加多 marker、prefix/suffix 混用、复合返回值等测试覆盖。
- 如有目标后端需求，再引入可选扩展 descriptor helper。
