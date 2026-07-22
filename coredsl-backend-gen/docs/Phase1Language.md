# Phase-1 CoreDSL frontend contract

This document freezes the syntax accepted by the standalone frontend.  It is a
parser contract only: it does not assign LLVM meaning, infer operands, or
validate instruction encodings.  The implementation uses LLVM Support source
and diagnostic facilities, but no LLVM IR, TargetMachine, CodeGen, or pass
pipeline.  Additions require parser regression coverage; rejected forms also
require a diagnostic fixture.

```text
instruction-set := "InstructionSet" identifier ["extends" identifier]
                   "{" "instructions" "{" instruction* "}" "}"
instruction     := identifier "{" instruction-member* "}"
instruction-member
                := "operands" [":"] "{" operand* "}"
                 | "encoding" [":" | "="] raw-value
                 | "assembly" [":" | "="] assembly-value ";"
                 | "behavior" [":"] statement
operand         := type identifier ";"
type            := identifier ["<" (identifier | integer) ">"]
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
`ExampleRV64.core_desc`; loops, target properties, and new instruction members
remain intentionally outside the Phase-1 contract.

The parser accepts `//` and `/* ... */` comments.  Every AST node carries a
source range, raw encoding tokens retain their locations, and diagnostics use
the form `file:line:column: error: message`.

Unsupported syntax is rejected with a deterministic diagnostic rather than
being interpreted as an LLVM-specific construct.
