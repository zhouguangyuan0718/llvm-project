# Phase-1 CoreDSL frontend contract

This document freezes the syntax accepted by the standalone frontend.  It is a
parser contract only: it does not assign LLVM meaning, infer operands, or
validate instruction encodings.  Additions require both an AST fixture and a
diagnostic fixture.

```text
instruction-set := "InstructionSet" identifier ["extends" identifier]
                   "{" "instructions" "{" instruction* "}" "}"
instruction     := identifier "{" instruction-member* "}"
instruction-member
                := "encoding" [":" | "="] raw-value
                 | "assembly" [":" | "="] string ";"
                 | "behavior" [":"] statement
                 | identifier [":" | "="] raw-value
```

`raw-value` is preserved as a source-ordered token sequence.  It may be a
braced block or a semicolon-terminated sequence.  This deliberately defers the
encoding grammar to the semantic-model milestone while allowing existing
CoreDSL encoding spellings to parse losslessly.

Behavior statements are blocks, expression statements, empty statements,
`if`/`else`, `while`, `for`, and declarations.  A declaration begins with
`let`, `var`, or two adjacent identifiers (a lightweight typed-declaration
form).  Expressions support identifiers, integer and string literals, calls,
indexing, member access, prefix/postfix increment/decrement, unary operators,
arithmetic, shifts, comparisons, bitwise/logical operators, conditional
expressions, and assignment/compound-assignment operators.

The parser accepts `//` and `/* ... */` comments.  Every AST node carries a
source range, raw encoding tokens retain their locations, and diagnostics use
the form `file:line:column: error: message`.

Unsupported syntax is rejected with a deterministic diagnostic rather than
being interpreted as an LLVM-specific construct.
