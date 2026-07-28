# LLVM 23 instruction `def` and `Pat` spike

The `--emit-td-spike` path tests how far the current frontend IR can lower
without an LLVM optimization or code-generation pipeline.  It emits a
self-contained TableGen validation unit so that `llvm-tblgen` can parse the
instruction definition and import the pattern into both SelectionDAG and
GlobalISel.

The current successful case is the fixed one-dimensional register tensor:

```coredsl
tensor<signed<16>, 64> dst[register];
tensor<signed<16>, 64> lhs[register];
tensor<signed<16>, 64> rhs[register];

dst = lhs + rhs;
```

The spike derives LLVM `v64i16`, a temporary `TensorReg_v64i16` register
class, one output, two inputs, and this pattern:

```tablegen
def : Pat<(v64i16 (add (v64i16 TensorReg_v64i16:$lhs),
                       (v64i16 TensorReg_v64i16:$rhs))),
          (TADD TensorReg_v64i16:$lhs, TensorReg_v64i16:$rhs)>;
```

LLVM 23 imports it as a GlobalISel rule that checks `G_ADD` and the generated
register class, then mutates the opcode to `TADD`.

## Information still required for real instruction definitions

The spike intentionally synthesizes four tensor registers and emits `TADD` as
a pseudo instruction.  A real non-pseudo `def` still needs a canonical target
model containing:

- physical register names, count, hardware encodings, aliases, subregisters,
  allocation order, spill size and alignment;
- register classes and register banks for each supported scalar or tensor
  machine value type;
- parsed instruction width and encoding-field bindings instead of the current
  raw encoding token sequence;
- explicit operand roles (`def`, `use`, `def-use`, immediate, address and
  implicit operand), including tied-operand constraints;
- canonical mnemonic/assembly operands and feature predicates;
- instruction effects and properties such as `mayLoad`, `mayStore`,
  `hasSideEffects`, commutability and scheduling metadata.

Simple single-assignment behavior can infer some operand roles, but that is not
enough for instructions with conditional writes, tied operands or hidden
state.  These facts belong in the canonical IR even when CoreDSL syntax allows
the frontend to infer them.

## Additional requirements for `Pat`

A fixed register tensor can use an LLVM fixed-vector MVT when both element type
and length are static.  Generating a useful pattern also requires:

- a canonical semantic operation such as add, multiply or a target accelerator
  operation;
- legalized LLTs/MVTs and a mapping to a register class and register bank;
- an unambiguous result expression and complete input/output operand mapping;
- a selection source for non-generic accelerator operations, normally an LLVM
  intrinsic or a target-specific Generic opcode;
- target feature predicates and any immediate or complex-operand matchers.

A dynamic memory tensor cannot be represented as a fixed vector MVT.  Its
machine instruction ABI must lower each logical tensor operand to at least a
base address plus the required runtime extents, and possibly strides, layout,
address space and alignment.  The semantic IR must also record which tensors
are read or written.  Only after that ABI and the intrinsic/Generic-op entry
are defined can the emitter generate an accurate memory-accelerator `def` and
selection pattern.

## Validation

Generate the spike:

```text
coredsl-backend-gen --emit-td-spike test/Parser/tensor.core_desc > tensor.td
```

Validate it independently with LLVM 23:

```text
llvm-tblgen -I llvm/include -gen-instr-info tensor.td
llvm-tblgen -I llvm/include -gen-dag-isel tensor.td
llvm-tblgen -I llvm/include -gen-global-isel tensor.td
```
