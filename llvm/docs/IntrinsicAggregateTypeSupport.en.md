# Intrinsic Aggregate Type Support Design (Struct Baseline + Final Array/MemRef Plan)

## 1. Background and Goals

This document summarizes the two-stage evolution:

1. **Baseline stage (Struct)**: add `struct` support to LLVM intrinsic type descriptions and make GlobalISel intrinsic matching work with aggregate signatures.
2. **Final stage (Array + MemRef)**: extend the same framework to `array`, and provide reusable memref-descriptor modeling plus rank-expansion helpers for MLIR-lowered memref descriptor scenarios.

Primary goals:

- Express aggregate types directly in intrinsic signatures (`struct` / `array`).
- Use a single **flattened leaf-type view** in TableGen / GlobalISel / DAG patterns.
- Reduce declaration duplication for memref intrinsic families across concrete ranks.

---

## 2. Baseline: Struct Support

### 2.1 Type-expression layer

The baseline introduces `LLVMStructType<...>` in `Intrinsics.td` to represent aggregate intrinsic parameter/return types. Encoding follows IIT conventions:

- Empty struct: `IIT_EMPTYSTRUCT`.
- Non-empty struct: `IIT_STRUCT` + element-count encoding + recursive element signatures.

### 2.2 GlobalISel import semantics

The baseline also removes assumptions that intrinsic aggregate arguments map to a single vreg. Expanded aggregate pieces can now be represented as multiple machine operands/uses.

### 2.3 TableGen semantics

A flatten-view model is introduced:

- Number of intrinsic params/results is based on flattened leaves, not only top-level `ParamTys`/`RetTys`.
- Pointer/ImmArg checks are mapped to flattened parameter indices.

This is the prerequisite for array/memref support.

---

## 3. Final Plan: Array + MemRef

### 3.1 First-class array support

#### 3.1.1 IIT and IR matching support

`IIT_ARRAY` / `IITDescriptor::Array` are added, and the IR intrinsic pipeline fully handles them:

- `DecodeIITType`: parse array length and element descriptors.
- `DecodeFixedType`: materialize LLVM `ArrayType`.
- `matchIntrinsicType`: recursively match length + element type.

So arrays are no longer ad-hoc; they are understood by intrinsic core infrastructure.

#### 3.1.2 TableGen flattening

`CodeGenIntrinsics` flattening is extended to:

- Recursively expand structs.
- Expand arrays by repeating element flattening `N` times.

Therefore `[2 x i32]` becomes two flattened `i32` operands.

### 3.2 MemRef descriptor modeling

`Intrinsics.td` defines fixed-layout `LLVMMemRefDescriptorType<rank>`:

```text
{ ptr allocatedPtr,
  ptr alignedPtr,
  i64 offset,
  [rank x i64] sizes,
  [rank x i64] strides }
```

Rank-fixed aliases are provided:

- `llvm_rank1_memref_ty` ... `llvm_rank5_memref_ty`.

### 3.3 Single declaration, multi-rank expansion

Marker + multiclass pattern:

- Marker type: `llvm_memref_ty`
- Expansion helper: `LLVMMemRefIntrinsic`

Example:

```tablegen
let TargetPrefix = "mytarget" in {
  defm int_mytarget_memref_add : LLVMMemRefIntrinsic<
      [], [llvm_memref_ty]>;
}
```

This generates:

- `int_mytarget_memref_add_rank1` ... `rank5`.

Internally `ReplaceMemRefMarker` replaces `llvm_memref_ty` with corresponding `llvm_rankN_memref_ty`.

### 3.4 Placement and organization

For maintainability:

- Memref type aliases are defined after base `llvm_*_ty` type aliases.
- `LLVMMemRefIntrinsic` is defined after `Intrinsic/DefaultAttrsIntrinsic`, since it expands to intrinsic definitions.

---

## 4. Key Trade-offs

### 4.1 Why match on flattened leaves?

Benefits:

- Matches MachineIR/GlobalISel operand reality.
- Keeps matcher/type-constraint logic uniform (no aggregate-only special case).
- Reuses existing pointer/ImmArg machinery.

Cost:

- One semantic high-level argument can become multiple matcher operands.
- Tests/patterns must use correct leaf types (for memref: `ptr`, `ptr`, then `i64...`).

### 4.2 Why fixed `ptr + i64` memref helper?

Benefits:

- Matches current MLIR lowered memref descriptor convention.
- Avoids template explosion and test complexity.
- Stable user model: marker replacement always yields canonical descriptor layout.

Cost:

- Future non-`i64` index or custom pointer variants require dedicated helper extensions.

---

## 5. Relation Between Baseline and Final Design

- The **struct baseline** establishes the aggregate flatten framework.
- The **final array/memref plan** is a direct extension of the same framework:
  - struct + array recursive flattening,
  - unified flattened checks in DAG/GlobalISel,
  - memref as a high-frequency modeled aggregate (struct+array composition).

This keeps the implementation coherent instead of introducing one-off memref-only paths.

---

## 6. Benefits of the Final Approach

1. **Higher expressiveness**: intrinsic signatures can natively model array and memref descriptor shapes.
2. **Predictable matching behavior**: flattened semantics avoid top-level-vs-leaf operand-count mismatches.
3. **Lower declaration cost**: one marker-based declaration expands into rank1..5 variants.
4. **Better maintainability**: IIT decoding, type matching, TableGen flattening, and DAG constraints evolve consistently.
5. **GlobalISel-friendly**: aggregates are lowered to leaf operands early, avoiding later aggregate splitting complexity.

---

## 7. Future Work

- Make rank expansion range configurable (instead of fixed 1..5).
- Keep fixed-layout helper as default, and add optional extension helpers for special targets if needed.
- Add more lit coverage for:
  - multiple memref markers in one intrinsic,
  - marker mixed with argument prefix/suffix,
  - return types combining struct/array/memref patterns.

