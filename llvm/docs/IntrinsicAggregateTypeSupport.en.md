# LLVM Intrinsic Aggregate Type Support Design

## 1. Background and Goals

This design introduces aggregate-type support for LLVM intrinsics across the full pipeline:

- `array` type description and IIT encode/decode/match support;
- memref descriptor modeling with rank-instantiated signatures;
- a unified flatten model for TableGen / GlobalISel / DAG patterns.

The goal is to make intrinsic signatures, type matching, and instruction-selection behavior consistent and predictable for aggregate types.

---

## 2. What changes relative to existing LLVM behavior

### 2.1 Intrinsic IIT extension for Array

Add `IIT_ARRAY` and `IITDescriptor::Array`, and wire them through:

- IIT decoding,
- fixed LLVM type materialization (`ArrayType`),
- intrinsic signature matching (recursive length + element-type checks).

This promotes array support to first-class intrinsic infrastructure.

### 2.2 TableGen type-expression extension

Add `LLVMArrayType<num_elements, element_type>` so fixed-size arrays can be expressed directly in intrinsic signatures.

Add `LLVMMemRefDescriptorType<rank>` to model MLIR-lowered memref descriptors with fixed layout.

### 2.3 Marker + macro expansion for memref intrinsic families

Add:

- marker type: `llvm_memref_ty`,
- concrete rank aliases: `llvm_rank1_memref_ty ... llvm_rank5_memref_ty`,
- replacement helper: `ReplaceMemRefMarker`,
- expansion multiclass: `LLVMMemRefIntrinsic`.

A single `defm` can generate rank1..5 intrinsic variants without manual duplication.

### 2.4 Unified flatten semantics across TableGen/GlobalISel/DAG

`CodeGenIntrinsics` flattening now recursively handles both struct and array:

- struct: recursive element expansion,
- array: repeat element expansion `N` times.

`CodeGenDAGPatterns` intrinsic checks and type application now use flattened parameter/result lists instead of top-level counts.

This removes common operand-count mismatches between signature-level and matcher-level views.

---

## 3. Design details

### 3.1 Array in IIT/IR path

Array handling is complete in three places:

1. `DecodeIITType`: parse length + element descriptor,
2. `DecodeFixedType`: build LLVM `ArrayType`,
3. `matchIntrinsicType`: recursive length and element matching.

### 3.2 MemRef descriptor model

`LLVMMemRefDescriptorType<rank>` uses fixed layout:

```text
{ ptr allocatedPtr,
  ptr alignedPtr,
  i64 offset,
  [rank x i64] sizes,
  [rank x i64] strides }
```

`rank` is a compile-time constant, aligned with LLVM intrinsic static typing.

### 3.3 Single declaration, multi-rank generation

Example:

```tablegen
defm int_memref_elem_add : LLVMMemRefIntrinsic<
    [], [llvm_memref_ty]>;
```

Auto-generates:

- `int_memref_elem_add_rank1` ... `rank5`.

### 3.4 Definition placement

- memref base aliases are placed after base `llvm_*_ty` type aliases,
- `LLVMMemRefIntrinsic` is placed after `Intrinsic/DefaultAttrsIntrinsic` definitions.

---

## 4. Advantages of the approach

1. **Higher expressiveness**: intrinsics can natively model array and memref-descriptor shapes.  
2. **Cross-layer consistency**: IIT decode, type matching, TableGen flattening, and DAG constraints share one semantic model.  
3. **Predictable matcher behavior**: avoids top-level-vs-leaf operand count mismatches.  
4. **Lower declaration cost**: marker + `defm` expands rank variants automatically.  
5. **Lower maintenance cost**: less repetitive rank-specific boilerplate.

---

## 5. Trade-offs

### 5.1 Flattened leaf matching

Pros:

- aligns with MachineIR operand reality,
- keeps matcher logic uniform.

Cost:

- tests/patterns must use correct leaf operand types (for memref: `ptr`, `ptr`, then `i64...`).

### 5.2 Fixed memref field types (`ptr + i64`)

Pros:

- aligned with current MLIR-lowered descriptor convention,
- avoids template explosion.

Cost:

- future field-type variants require explicit extension helpers.

---

## 6. Recommended follow-ups

- make rank set configurable (instead of fixed 1..5),
- add coverage for multiple markers, marker+prefix/suffix mixing, and composite return cases,
- introduce optional extension helpers only when real backend needs appear.
