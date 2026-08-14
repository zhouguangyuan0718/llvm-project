# LLVM 23 GlobalISel legalizer templates

This package generates one target-specific `LegalizerInfo` class. It is
deliberately independent of the CoreDSL frontend and can be rendered by a
Mustache implementation that supports the standard set-delimiter tag.

The templates cover two mechanisms:

1. same-size representation rewrites at a `G_INTRINSIC` boundary, such as
   `f16` to `i16`, implemented with `G_BITCAST`; and
2. replacement of the generic `i1` condition with a target-supported carrier
   type across compare producers and selected condition consumers.

They do not generate numeric conversions, general floating-point legalization,
instruction selection, or register-bank mappings.

## Files

- `LegalizerInfo.h.mustache` generates the class declaration.
- `LegalizerInfo.cpp.mustache` generates rules and intrinsic legalization.
- `legalizer-input.schema.json` defines the renderer input contract.
- `legalizer-input.example.json` demonstrates `f16 <-> i16` and an `i16`
  condition carrier.

The templates switch Mustache delimiters to `<%` and `%>` so C++ initializer
lists containing `{{...}}` remain literal C++.

## Input contract

The producer must normalize its target model before rendering. In particular:

- every `*_type_cpp` value is a concrete LLVM C++ expression such as
  `LLT::integer(16)` or `LLT::float16()`;
- `has_subtarget` controls whether the generated constructor receives a
  `subtarget_class_name` reference named `ST`; generated type expressions may
  inspect it, but the legalizer does not retain the reference;
- `bitcast_pairs` contains every ordered `{destination, source}` pair created
  by intrinsic rewrites, with duplicate pairs removed;
- each intrinsic rewrite uses a MIR operand index, not an LLVM IR argument
  index;
- every rewrite states the expected source representation and the required
  target representation;
- the producer rejects duplicate intrinsic IDs and duplicate operand indices
  within one intrinsic rewrite list;
- `is_def` must agree with the referenced `MachineOperand`; and
- `has_fcmp` and `has_select` mirror whether their corresponding arrays are
  non-empty. This duplication keeps the template logicless and portable across
  Mustache implementations.

For a one-definition `G_INTRINSIC`, operand 0 is the definition, operand 1 is
the intrinsic ID, and operand 2 is the first input. With multiple definitions,
the intrinsic ID is at `getNumExplicitDefs()` and the inputs follow it. Compute
the final MIR operand indices in the input producer.

The generated intrinsic helper validates every rewrite before mutating the
instruction. It accepts an operand only when its exact LLT is either the stated
source type or the already-legal target type, and it rejects width-changing
bitcasts. Unknown intrinsics follow `intrinsics.default_result_cpp`; use
`false` for fail-closed generated targets.

## Condition closure

`G_ICMP` and `G_FCMP` use type index 0 for the condition result and type index
1 for compared values. `G_BRCOND` uses type index 0 for its condition, while
`G_SELECT` uses type index 1. The generated rules therefore rewrite the same
source condition type consistently at each enabled boundary.

Enable `emit_phi` only when this template owns the target's complete `G_PHI`
policy. Most targets should instead add the legal condition carrier to their
existing `G_PHI` rules, because `G_PHI` also transports non-condition values.

The `*_unsupported` switches append a catch-all rule for that opcode. Set them
only when this generated block owns all legalization rules for the opcode;
otherwise leave them false and let the surrounding target rules provide the
fallback.

The condition convention expected by this prototype is false=`0`, true=`1`,
with branch consumers treating any nonzero value as true. A target using a
predicate register or an all-ones mask needs a different custom legalization.

## Integration checks

After rendering, validate at least these layers:

1. JSON input against `legalizer-input.schema.json`;
2. compile the generated header and source against the exact LLVM payload;
3. run a MIR legalizer test and verify the intrinsic has same-size `G_BITCAST`
   boundaries and the compare/consumer use the configured condition type; and
4. run instruction selection and verify `G_BITCAST`, `G_INTRINSIC`, compare,
   and condition consumers are all selected or intentionally eliminated.

The generator must also emit matching legality/selection and register-bank
support for each ordered `bitcast_pairs` entry. Declaring `G_BITCAST` legal in
this template alone does not make it selectable.
