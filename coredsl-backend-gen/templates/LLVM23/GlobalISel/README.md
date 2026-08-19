# LLVM 23 GlobalISel legalizer templates

This package generates one target-specific `LegalizerInfo` class. The renderer
input contains one target name, target-native scalar types, the scalar types
supported by ordinary load/store instructions, and intrinsic scalar argument
index/type rules. File names, the class name, the include guard, the `llvm`
namespace, and all legalization policies are derived or built into the
templates.

The templates use standard `{{...}}` tags and can be rendered directly with
`llvm::mustache::Template` from `LLVMSupport`. A C++ example is provided in
`llvm-api-render-example.cpp`. `TARGET_INTEGRATION.md` shows how another LLVM
Target compiles and exposes the generated legalizer to GlobalISel.

## Renderer input

The complete input shape is:

```json
{
  "target": "Example",
  "native_types": {
    "integer_widths": [16, 32],
    "floating_point_widths": [16, 32]
  },
  "memory_types": {
    "integer_widths": [16, 32],
    "floating_point_widths": [32]
  },
  "intrinsics": [
    {
      "id_cpp": "Intrinsic::example_f16_op",
      "scalar_arguments": [
        {
          "index": 0,
          "types": [
            { "integer_width": 16 },
            { "integer_width": 32 }
          ]
        }
      ]
    }
  ]
}
```

`target` is the single naming input. For `"Example"`, the templates derive:

- class: `ExampleLegalizerInfo`;
- header: `ExampleLegalizerInfo.h`;
- include guard: `LLVM_LIB_TARGET_EXAMPLE_LEGALIZERINFO_H`;
- intrinsic header: `llvm/IR/IntrinsicsExample.h`;
- C++ namespace: always `llvm`.

The target string must be a valid C++ identifier component and a valid file-name
component. The input producer should use the exact LLVM Target spelling because
Mustache does not perform case conversion.

The LLVM API renderer registers a `target_upper` lambda derived from `target`:

```cpp
Template.registerLambda("target_upper", [Upper = Target.upper()]() {
  return llvm::json::Value(Upper);
});
```

The header template uses `{{target_upper}}` only for its include guard. This is
renderer-derived state; callers still provide only the single `target` field.

`integer_widths` must be non-empty, unique, and strictly increasing. A missing
integer width is promoted to the narrowest wider native integer. A value wider
than the largest native integer is not handled; this policy never narrows or
splits integers.

`floating_point_widths` must contain only the supported IEEE widths `16`, `32`,
`64`, and `128`; entries must be unique and strictly increasing. Each width is
converted to an exact ExtendedLLT type with `LLT::floatIEEE(N)`. Only listed
types are accepted by numerical floating-point opcodes. An empty array
represents a target without floating-point register types. Non-IEEE formats
such as `bf16`, x87 extended precision, and PPC double-double are intentionally
outside this compact input contract.

`memory_types` is the common capability set for plain non-atomic scalar
`G_LOAD` and `G_STORE`. Its integer and floating-point widths use the same
strict ordering and type spelling rules as `native_types`; every directly legal
memory type must also be present in the corresponding native-type list. Only
integer and floating-point capabilities need configuration. Exact pointer-
valued loads and stores are directly legal without a `memory_types.pointer`
input. The load/store address operand is also a pointer and is preserved. Value
and MMO memory types must be identical; the generated policy does not infer
extending loads or truncating stores.

A floating-point memory operation not listed in
`memory_types.floating_point_widths` may still be represented by an equal-width
integer only when that exact integer width is present in both
`native_types.integer_widths` and `memory_types.integer_widths`. The legalizer
bitcasts the value and changes the MMO type without changing its bit width.

These templates assume that the ExtendedLLT reland is cherry-picked onto
`llvmorg-23-init` and enabled consistently by the integrating target. Integer,
floating-point, and generic `sN` types then have different identities. The
generated legalizer deliberately uses `LLT::integer(N)` and
`LLT::floatIEEE(N)`; do not replace them with the generic `LLT::scalar(N)`
wildcard.

Each intrinsic entry contains:

- `id_cpp`: the C++ intrinsic ID used in the generated `switch`;
- `scalar_arguments`: per-argument rules containing a zero-based explicit
  input `index` and a non-empty ordered `types` list.

The argument index excludes definitions and the intrinsic-ID operand. Every
entry in `types` must contain exactly one of:

```json
{ "integer_width": 16 }
{ "floating_point_width": 32 }
```

Every candidate type must occur in the corresponding `native_types` list. An
integer candidate enables the carrier conversions below. A floating-point
candidate currently matches only an actual argument with that exact native
floating-point type; the template does not infer numeric integer/float
conversions.

Candidate selection is deterministic:

1. Preserve an exact actual type, regardless of candidate order.
2. Prefer an equal-width floating-point-to-integer representation change.
3. For all remaining widening and constant-narrowing conversions, select the
   first convertible candidate in the declared `types` order.

For example, actual `i32` with candidates `[i16, i32]` remains `i32`; actual
`f16` with `[i32, i16]` selects the equal-width `i16`; and actual `i8` with
`[i16, i32]` selects `i16`.

For example, argument index `0` is mapped at legalization time to MIR operand
`MI.getNumExplicitDefs() + 1`. The runtime helper verifies that every listed
argument is a convertible virtual-register use before changing any operand.
Duplicate intrinsic IDs and duplicate argument indices must be rejected by the
input producer. Empty candidate lists and duplicate candidate types must also
be rejected. Unknown intrinsics return `false`.

For an intrinsic argument whose selected type is `iM`, the generated boundary
is:

```text
nonconstant fN, N <= M -> G_BITCAST iN  -> G_ANYEXT iM -> intrinsic
G_FCONSTANT fN, N <= M -> G_CONSTANT iN -> G_ANYEXT iM -> intrinsic
iN, N < M --------------------------------------> G_ANYEXT iM -> intrinsic
G_CONSTANT iN, N > M and value fits ------------> G_CONSTANT iM -> intrinsic
```

Extensions pass through the narrowest native carrier when needed, so every
generated artifact remains covered by the built-in extension rules. Thus a
half constant requested as `i16` becomes one `G_CONSTANT i16` whose value is the
exact `APFloat::bitcastToAPInt()` result; no `G_BITCAST` is emitted. The original
`G_FCONSTANT` is removed by normal dead-instruction handling when it has no
other non-debug users. `G_ANYEXT` preserves only a low-bit payload convention;
a numerically signed or unsigned widening needs a different semantic rule.

A wider integer is accepted only when it is defined directly by `G_CONSTANT`
and its value is representable in a candidate width. LLVM integer types are
signless, so the test accepts a value when truncation can be reversed by either
zero extension or sign extension. For `i16`, both `65535` and `-1` are accepted
and materialized as `0xffff`; `65536` tries the next candidate instead. With
`[i16, i32]`, `70000` therefore selects `i32`, while a value outside both ranges
is rejected. Nonconstant wider integers are never implicitly truncated. The
normal bottom-up legalizer order rewrites the intrinsic use before visiting its
constant definition; an original wide constant with no remaining uses is then
removed as dead.

## Built-in opcode policy

The following scalar and pointer-carrier rules are always emitted:

- value carriers: `G_IMPLICIT_DEF`, `G_FREEZE`,
  `G_CONSTANT_FOLD_BARRIER`; pointer values are accepted directly, while
  integer and floating-point values must use native types;
- constants: native-width integer and pointer `G_CONSTANT`;
- integer type index 0: `G_ADD`, `G_SUB`, `G_MUL`, `G_SDIV`,
  `G_UDIV`, `G_SREM`, `G_UREM`, `G_AND`, `G_OR`, `G_XOR`, `G_SMIN`,
  `G_SMAX`, `G_UMIN`, `G_UMAX`, `G_ABS`, `G_SEXT_INREG`, `G_BSWAP`,
  `G_BITREVERSE`;
- integer type indices 0 and 1: `G_SHL`, `G_LSHR`, `G_ASHR`;
- integer result and input types: `G_CTLZ`, `G_CTLZ_ZERO_UNDEF`, `G_CTTZ`,
  `G_CTTZ_ZERO_UNDEF`, `G_CTPOP`;
- floating-point constants and native floating-point operations:
  `G_FCONSTANT`, `G_FADD`, `G_FSUB`, `G_FMUL`, `G_FDIV`, `G_FREM`, `G_FMA`,
  `G_FMAD`, `G_FNEG`, `G_FABS`, `G_FCANONICALIZE`, all standard
  `G_FMIN*`/`G_FMAX*` variants, `G_FSQRT`, `G_FCEIL`, `G_FFLOOR`, `G_FRINT`,
  `G_FNEARBYINT`, `G_FCOPYSIGN`;
- scalar casts: native integer `G_ANYEXT`/`G_ZEXT`/`G_SEXT`/`G_TRUNC` pairs,
  native `G_FPEXT`/`G_FPTRUNC` pairs, and `G_SITOFP`, `G_UITOFP`, `G_FPTOSI`,
  `G_FPTOUI` when their floating-point side is native;
- pointer representation: `G_PTR_ADD`, `G_INTTOPTR`, `G_PTRTOINT`; their
  integer operand or result follows the native integer-width policy;
- direct pointer carriers: `G_FRAME_INDEX`, `G_GLOBAL_VALUE`,
  `G_CONSTANT_POOL`, `G_BLOCK_ADDR`, `G_JUMP_TABLE`, `G_BRINDIRECT`;
- untyped unconditional control flow: `G_BR`;
- plain non-atomic scalar memory: `G_LOAD`, `G_STORE`;
- condition and consumers: `G_ICMP`, `G_FCMP`, `G_BRCOND`, `G_SELECT`, `G_PHI`.

The smallest native integer is the default condition carrier where there is no
integer value type to follow. For integer `G_ICMP`, the result uses the same
native carrier as the legalized inputs: with native integers `[16, 32]`, both
`G_ICMP (i1, i8)` and `G_ICMP (i1, i16)` become `G_ICMP (i16, i16)`, while
`G_ICMP (i1, i32)` becomes `G_ICMP (i32, i32)`. The result carrier is computed
from the original input width so a missing input width does not first become an
unsupported result width. Pointer inputs cannot be used as a result type and
therefore retain the smallest native integer result while their pointer type is
unchanged. Non-integer results, non-integer/non-pointer inputs, result types
wider than the required carrier, and widths exceeding every native integer fail
closed. `G_FCMP` promotes only its result and accepts only native floating-point
inputs. The condition input of `G_SELECT` uses the smallest condition carrier.
Pointer values are also accepted by `G_SELECT` and `G_PHI`.

When the target declares `ZeroOrOneBooleanContent`, legalization artifacts from
an original `icmp -> zext -> store` chain can be combined away after widening.
For an input and store carrier of `i16`, the legal form is one `G_ICMP i16`
followed by `G_STORE i16`; no mask with constant one is required. Other boolean
content conventions cannot make that promise because the mask may be needed to
preserve the original zero-extension semantics.

`G_BRCOND` accepts every native integer condition directly. A condition whose
integer width is missing is promoted to the narrowest native integer at least
as wide as the original condition; LLVM's generic legalizer uses its boolean
extension operation for this rewrite. This covers both the usual promoted `i1`
condition and branches driven by an already-native wider integer. Non-integer
conditions and integer conditions wider than every native type fail closed.

`G_PTR_ADD` preserves its pointer type. A missing integer offset width is
promoted to the narrowest native integer with the generic legalizer's signed
extension, matching pointer-offset semantics. `G_INTTOPTR` similarly promotes
its integer input, using zero extension; `G_PTRTOINT` promotes a missing integer
result width and leaves a truncation for users of the original result.

The direct pointer-carrier group has no type mutation because pointer types are
not part of this compact native scalar input. Declaring these GMIR operations
legal only means that type legalization is complete. The target's instruction
selector must still implement its frame, relocation/code-model, constant-pool,
block-address, jump-table, and indirect-branch forms.

`G_BR` has no register type to legalize, so it is always legal at this layer.

`G_CONSTANT` uses the same integer-width policy: a native-width constant is
legal, while a missing integer width is promoted to the smallest wider native
integer. LLVM's generic legalizer handles the corresponding constant widening.

A native `G_FCONSTANT` is legal. An unsupported floating-point constant with
an available integer carrier is folded directly into an integer constant:

```text
%dst:fN = G_FCONSTANT value
%bits:iN = G_BITCAST %dst
  ->
%bits:iN = G_CONSTANT bitcast(value)
```

The bitcast result register and all of its uses are preserved; the
`G_FCONSTANT` and `G_BITCAST` are removed. Every non-debug use of the
unsupported floating-point constant must be an equal-width integer bitcast.
The resulting `G_CONSTANT iN` is promoted by the integer-width policy when
`iN` itself is not native. Unsupported numerical floating-point operations
still fail closed; they are not reinterpreted as integer arithmetic.

`G_SELECT` may bitcast an unsupported floating-point selected value to an
equal-width integer and then promote that integer. This is representation-safe
for selection and is not applied to numerical floating-point operations.
`G_PHI` promotes integer values but does not bitcast unsupported float values.

The templates also mark the artifacts introduced by this policy as legal:
integer extensions to a native carrier, the inverse truncations, and equal-width
integer/floating-point bitcasts. Native integer and floating-point conversion
pairs listed above are legal as well. No generated rule ends in a catch-all
`unsupported()`, so a surrounding target may add pointer, vector, memory,
multi-result, or opcode-specific rules.

Plain non-atomic scalar `G_LOAD` and `G_STORE` require identical value and MMO
memory types. An integer or floating-point operation is directly legal only
when its type is present in both the corresponding native-register list and
the corresponding `memory_types` list. An exact pointer-valued operation is
directly legal without a separate capability field. The address pointer
operand, alignment, ordering, and other MMO flags are preserved.

When a floating-point type is not directly supported but the exact same-width
integer is both native and memory-supported, the representation is changed
without changing the access width. LLVM clears load range metadata because the
old floating-point range is no longer valid:

```text
G_LOAD  fN              -> G_LOAD iN; G_BITCAST iN to fN
G_STORE fN              -> G_BITCAST fN to iN; G_STORE iN
```

Here `fN` and `iN` have identical bit widths. There is deliberately no integer
widening rule for memory operations: an unsupported `G_LOAD/G_STORE i8` does
not become a mismatched `value i16, memory i8` operation. When `fN` is also not
a native floating-point register type, a stored `G_FCONSTANT` can subsequently
fold with its generated bitcast into a `G_CONSTANT`, using the same
constant-carrier rule described above.

This built-in rule is deliberately limited to one-MMO, non-atomic scalar
operations with exact value/memory type identity. Atomic, indexed, vector,
multi-MMO, and mismatched value/memory operations fall back to the target's
other rules rather than being inferred from register types.

The audit deliberately does not mark `G_ADDRSPACE_CAST`, `G_PTRMASK`,
`G_DYN_STACKALLOC`, `G_STACKSAVE`, `G_STACKRESTORE`, `G_BRJT`, atomic memory
operations, or `G_VAARG`/`G_VASTART` directly legal. Those require address-space,
pointer-width, stack/ABI, jump-table, atomic, or varargs policy that cannot be
derived from `native_types`. Optimization hints such as `G_ASSERT_ZEXT`,
`G_ASSERT_SEXT`, and `G_ASSERT_ALIGN` are outside the normal pre-isel generic
opcode legality range and are eliminated later; they need no generated rule.

At `llvmorg-23-init`, `LegalizerInfo` still falls back to its embedded
`LegacyLegalizerInfo` when no modern rule matches. The generated constructor
therefore finishes with `getLegacyLegalizerInfo().computeTables()`. This is
required even when the target adds no legacy actions; omitting it triggers the
`TablesInitialized` assertion on the first fallback query.

Declaring a native type does not prove instruction-selector coverage. Every
built-in opcode that can reach legalization must still be selected, lowered,
or otherwise eliminated by the target.

## Rendering with LLVM APIs

`llvm-api-render-example.cpp` constructs `llvm::json::Object` data directly and
renders a template with `llvm::mustache::Template`. It deliberately disables
the renderer's default HTML escaping because the output is C++ source.

When integrating the renderer as a CMake target, link it with:

```cmake
target_link_libraries(your-legalizer-generator PRIVATE LLVMSupport)
```

An integration can render the two files independently:

```sh
llvm-api-render-example LegalizerInfo.h.mustache > ExampleLegalizerInfo.h
llvm-api-render-example LegalizerInfo.cpp.mustache > ExampleLegalizerInfo.cpp
```

The example JSON is only a compact description of the input shape. An
LLVM-based input producer does not need to serialize through JSON text.

## Integration checks

1. Validate the target spelling, duplicate intrinsic IDs and argument indices,
   non-empty candidate lists, duplicate candidates, exactly one type variant
   for every candidate, and that every candidate type is native. Also validate
   all strictly increasing integer-width lists and all strictly increasing IEEE
   floating-point lists drawn from `16`, `32`, `64`, and `128`. Direct
   memory-type lists must be subsets of their corresponding native-type lists.
2. Disable HTML escaping before rendering C++ values.
3. Run `clang-format` on the generated source.
4. Compile against the exact LLVM payload.
5. Confirm that target TableGen uses `-gisel-extended-llt` and target runtime
   setup calls `LLT::setUseExtended(true)` before GlobalISel creates LLTs.
6. Test every native integer, every gap below the largest integer, native and
   unsupported floating-point types, and a width above the largest integer.
7. Inspect post-legalization MIR and run instruction selection for every
   generated artifact, intrinsic, comparison, and consumer.
