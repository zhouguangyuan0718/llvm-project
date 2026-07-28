# Phase-1 CoreDSL frontend contract

This document freezes the syntax accepted by the standalone frontend.  It is a
parser contract only: it does not assign LLVM meaning, infer operands, or
validate instruction encodings.  The implementation uses LLVM Support source
and diagnostic facilities, but no LLVM IR, TargetMachine, CodeGen, or pass
pipeline.  Additions require parser regression coverage; rejected forms also
require a diagnostic fixture.

```text
instruction-set := "InstructionSet" identifier ["extends" identifier]
                   "{" [target] "instructions" "{" instruction* "}" "}"
target          := "target" "{" target-property* "}"
target-property := identifier ":" (identifier | integer | string) ";"
instruction     := identifier "{" instruction-member* "}"
instruction-member
                := "operands" [":"] "{" operand* "}"
                 | "encoding" [":" | "="] raw-value
                 | "assembly" [":" | "="] assembly-value ";"
                 | "behavior" [":"] statement
operand         := type identifier [tensor-storage] ";"
type            := scalar-type | tensor-type
scalar-type     := identifier ["<" type-parameter ">"]
tensor-type     := "tensor" "<" scalar-type
                   "," dimension ("," dimension)* ">"
tensor-storage  := "[" ("register" | "memory") "]"
type-parameter  := identifier | integer
dimension       := identifier | integer
                 | "*"
assembly-value  := string | "{" string ("," string)* "}"
```

`raw-value` is preserved as a source-ordered token sequence.  It may be a
braced block or a semicolon-terminated sequence.  In the current CoreDSL
subset, `encoding` uses a semicolon-terminated sequence of Verilog-style
literals (for example `7'b0101000`), named fields (for example `rs1[4:0]`),
and `::` concatenation.  This deliberately defers encoding semantics to the
semantic-model milestone while preserving the existing spelling losslessly.

Behavior statements are blocks, typed declarations, expression statements,
empty statements, and `if`/`else`.  Expressions support identifiers, integer
literals, typed casts, indexed architectural-state access, bit slices such as
`X[rs1][31:0]`, unary operators, arithmetic, shifts, comparisons,
bitwise/logical operators, conditional expressions, and assignment or
compound-assignment operators.  This covers the reference
`core_descs/ExampleRV32.core_desc`, `ExampleRV32K.core_desc`, and
`ExampleRV64.core_desc`; loops and new instruction members remain intentionally
outside the Phase-1 contract.

Tensor types are first-class operand and local-declaration types. A tensor
type records its scalar element type and source-ordered shape dimensions. Each
tensor declarator carries an explicit postfix storage qualifier, for example
`tensor<signed<16>, 64> value[register]`. The `register` form requires exactly
one dimension. The `memory` form accepts any positive rank and may use `*` for
a dimension whose extent is only known at runtime. Register tensors do not
accept dynamic dimensions. Both forms use
the same expression nodes; storage-specific ABI lowering and tensor operation
type checking belong to later semantic-model milestones.

The parser accepts `//` and `/* ... */` comments.  Every AST node carries a
source range, raw encoding tokens retain their locations, and diagnostics use
the form `file:line:column: error: message`.

Unsupported syntax is rejected with a deterministic diagnostic rather than
being interpreted as an LLVM-specific construct.

## Minimal backend-generation target block

The optional `target` block is the first additive CoreDSL extension used by
the standalone backend generator.  It supplies facts which cannot be inferred
from instruction behavior and therefore must not be hidden in LLVM-side
mapping files:

```coredsl
target {
  llvm_name: "tiny32";
  namespace: Tiny32;
  register_prefix: "r";
  register_count: 8;
  register_width: 32;
  register_class: GPR;
  register_bank: GPRBank;
}
```

For the initial GlobalISel-only subset, registers are named by concatenating
`register_prefix` and their zero-based encoding.  The model validates these
seven properties and derives one integer register class and one register bank.
All generated instructions are pseudo instructions: neither MC encoding nor
binary emission is claimed by this contract. `--emit-llvm23-backend` also
emits the minimal LLVM C++ scaffolding required to translate simple LLVM IR,
legalize supported scalar operations, select their register bank, and execute
GlobalISel instruction selection. Its intentionally narrow ABI maps
register-width scalar arguments to generated physical registers and zero or
one scalar return value to register zero. It does not provide calls, stack
arguments, an assembler, object writer, complete calling convention, or
register-allocation pipeline.
