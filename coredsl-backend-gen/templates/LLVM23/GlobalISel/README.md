# LLVM 23 GlobalISel legalizer templates

This package generates one target-specific `LegalizerInfo` class. It is
independent of the CoreDSL frontend and can be rendered by a Mustache
implementation that supports the standard set-delimiter tag.

The type policy is derived from one target-native type profile:

1. a native scalar integer or floating-point type stays unchanged;
2. a missing integer width is promoted to the narrowest wider native integer;
3. a listed intrinsic floating-point operand is bitcast to an equal-width
   integer and then promoted when that integer width is not native; and
4. compare results and their enabled consumers use the smallest native integer
   as the condition carrier.

No intrinsic, condition, or opcode rule repeats source/destination types. The
only type-related renderer input is `native_types`.

## Files

- `LegalizerInfo.h.mustache` generates the class declaration.
- `LegalizerInfo.cpp.mustache` generates the native-type policy, opcode rules,
  and intrinsic legalization.
- `legalizer-input.schema.json` defines the renderer input contract.
- `legalizer-input.example.json` demonstrates a target with native `i16/i32`,
  no floating-point register type, an `f16` intrinsic boundary, and an `i16`
  condition carrier.

The templates switch Mustache delimiters to `<%` and `%>` so C++ initializer
lists containing `{{...}}` remain literal C++.

## Native type input

`native_types` has two fields:

```json
{
  "integer_widths": [16, 32],
  "floating_point_types": ["LLT::float32()"]
}
```

The input producer must validate that:

- `integer_widths` is non-empty, unique, and strictly increasing;
- every integer entry is a real target register carrier width;
- every floating-point entry is a concrete ExtendedLLT expression;
- every listed floating-point type is directly selectable by the target; and
- the LLVM target enables ExtendedLLT consistently and produces concrete
  `LLT::integer()`/`LLT::float*()` types rather than wildcard scalar types.

The generated `getNativeIntegerTypeForWidth` function performs the promotion
lookup. A value wider than the largest native integer is rejected; this
prototype never narrows or splits it.

## Opcode input is not a type input

Native types do not prove that an opcode is implemented. The renderer therefore
still receives opcode coverage separately:

```json
{
  "opcode_cpp": "G_ADD",
  "type_indices": [0],
  "unsupported": true
}
```

`integer_opcode_rules` promotes each listed integer type index independently.
`floating_opcode_rules` accepts only native floating-point types and does not
turn floating-point arithmetic into integer arithmetic. For example, changing
`G_FADD f16` into `G_ADD i16` would change semantics and is never generated.

Use `type_indices: [0, 1]` for an opcode such as a shift whose value and amount
types are separate legality-query entries. Do not list `G_ICMP`, `G_FCMP`,
`G_BRCOND`, `G_SELECT`, `G_PHI`, or the cast/extension artifacts here; their
shapes are handled by dedicated generated rules.

The input producer must reject duplicate `opcode_cpp` entries and validate
each `type_indices` list against the opcode's actual `LegalityQuery` shape.

The `unsupported` switch appends a catch-all rule. Set it only when this block
owns the complete policy for that opcode.

## Intrinsic carrier convention

An intrinsic entry contains only its ID and the MIR operands that opt into the
integer-carrier convention:

```json
{
  "id_cpp": "Intrinsic::example_f16_op",
  "operands": [
    { "operand_index": 0, "is_def": true },
    { "operand_index": 2, "is_def": false }
  ]
}
```

For a one-definition `G_INTRINSIC`, operand 0 is the definition, operand 1 is
the intrinsic ID, and operand 2 is the first input. With multiple definitions,
the ID is at `getNumExplicitDefs()` and inputs follow it. The input producer
must reject duplicate intrinsic IDs and duplicate operand indices.

For a listed use, the generated sequence is:

```text
fN -> G_BITCAST iN -> G_ANYEXT iM -> intrinsic
```

The extension is omitted when `iN` is native. For a listed definition, the
inverse boundary is:

```text
intrinsic iM -> G_TRUNC iN -> G_BITCAST fN
```

Integer operands omit the bitcast and are only promoted/truncated. The helper
validates every operand and carrier before changing the instruction, so an
unsupported width cannot leave a partially legalized intrinsic.

`G_ANYEXT` means this convention preserves a low-bit payload, not signed or
unsigned numeric extension. An instruction that observes the promoted upper
bits needs a separate per-operand semantic policy and must not use this rule.

Unknown intrinsics follow `intrinsics.default_result_cpp`; generated targets
should normally use `false` and fail closed.

## Conditions and representation-safe float bitcasts

The smallest native integer is the generated condition carrier. `G_ICMP`
promotes both its result and integer operands. Enabled `G_BRCOND`, `G_SELECT`,
and `G_PHI` rules consume the same integer policy. `G_FCMP` is legal only for
native floating-point operand types; only its condition result is promoted.

`G_SELECT` is representation-safe, so a non-native floating-point selected
value may be bitcast to equal-width integer bits and then promoted. This is not
extended to arbitrary opcodes. `G_PHI` only promotes integer types because the
generic LegalizerHelper has no corresponding scalar PHI bitcast action.

The condition convention is false=`0`, true=`1`, with branch consumers treating
nonzero as true. Predicate registers or all-ones masks require a different
custom policy.

## Generated artifacts

The template derives legality predicates for only the artifacts it introduces:

- `G_ANYEXT/G_ZEXT/G_SEXT` from a missing integer width to its carrier;
- the inverse `G_TRUNC`; and
- equal-width integer/floating-point `G_BITCAST` boundaries.

`artifact_rules.unsupported` should normally remain false when the surrounding
target has additional pointer, vector, or cast rules. Legalizing these generic
artifacts still requires matching instruction selection and register-bank
support; type legalization alone does not make them selectable.

## Integration checks

After rendering:

1. validate JSON against `legalizer-input.schema.json` and validate the sorted
   integer-width invariant in the input producer;
2. run `clang-format` on the generated C++;
3. compile it against the exact LLVM payload;
4. test native integers, every gap below the maximum integer width, each
   supported/unsupported floating-point width, and a width above the maximum;
5. inspect MIR after legalization for the expected bitcast/extend/trunc order;
   and
6. run instruction selection to verify every generated artifact, intrinsic,
   compare, and consumer is selected or intentionally eliminated.
