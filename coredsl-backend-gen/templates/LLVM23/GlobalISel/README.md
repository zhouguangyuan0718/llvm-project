# LLVM 23 GlobalISel legalizer templates

This package generates one target-specific `LegalizerInfo` class. The renderer
input contains one target name, target-native scalar types, and one operation
type-constraint table shared by generic opcodes and intrinsics. File names, the
class name, the include guard, the `llvm` namespace, and all legalization
policies are derived or built into the templates.

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
  "operation_type_constraints": [
    {
      "opcode_cpp": "G_LOAD",
      "scalar_types": [
        {
          "index": 0,
          "types": [
            { "integer_width": 16 },
            { "integer_width": 32 },
            { "floating_point_width": 32 }
          ]
        }
      ]
    },
    {
      "opcode_cpp": "G_STORE",
      "scalar_types": [
        {
          "index": 0,
          "types": [
            { "integer_width": 16 },
            { "integer_width": 32 },
            { "floating_point_width": 32 }
          ]
        }
      ]
    },
    {
      "opcode_cpp": "G_MUL",
      "scalar_types": [
        {
          "index": 0,
          "types": [{ "integer_width": 16 }]
        }
      ]
    },
    {
      "opcode_cpp": "G_FDIV",
      "scalar_types": [
        {
          "index": 0,
          "types": [{ "floating_point_width": 32 }]
        }
      ]
    },
    {
      "opcode_cpp": "G_SHL",
      "scalar_types": [
        {
          "index": 0,
          "types": [
            { "integer_width": 32 },
            { "integer_width": 16 }
          ]
        },
        {
          "index": 1,
          "types": [{ "integer_width": 16 }]
        }
      ]
    },
    {
      "intrinsic_id_cpp": "Intrinsic::example_f16_op",
      "scalar_types": [
        {
          "index": 0,
          "types": [
            { "integer_width": 32 },
            { "integer_width": 16 }
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

`integer_widths` must be non-empty and unique. Input order has no semantic
effect. A missing integer width is promoted to the narrowest wider native
integer. A value wider than the largest native integer is not handled; this
policy never narrows or splits integers.

`floating_point_widths` must contain only the supported IEEE widths `16`, `32`,
`64`, and `128`; entries must be unique, and their order has no semantic effect.
Each width is converted to an exact ExtendedLLT type with `LLT::floatIEEE(N)`.
Listed types are directly legal. An unlisted IEEE type used by `G_FADD`,
`G_FSUB`, `G_FMUL`, `G_FDIV`, or as the input of `G_FCMP` is promoted to the
narrowest listed type with a greater width. A type wider than every listed type
is not handled. An empty array represents a target without floating-point
register types.
Non-IEEE formats such as `bf16`, x87 extended precision, and PPC double-double
are intentionally outside this compact input contract.

This numeric promotion policy is for ordinary, non-constrained GMIR floating-
point operations. It does not promise constrained-FP exception behavior,
dynamic rounding-mode equivalence, or strict avoidance of double rounding.

`operation_type_constraints` is the common capability table. Every entry must
contain exactly one operation identifier:

```json
{ "opcode_cpp": "G_MUL", ... }
{ "intrinsic_id_cpp": "Intrinsic::example_f16_op", ... }
```

Both forms contain a non-empty `scalar_types` array. Each scalar position has
an `index` and a non-empty `types` array. Every candidate contains exactly one
type spelling:

```json
{ "integer_width": 16 }
{ "floating_point_width": 32 }
```

Every candidate must occur in the matching `native_types` list. Duplicate
operation identifiers, duplicate indices within one operation, duplicate
candidate types, entries containing both/neither identifiers, and empty arrays
must be rejected by the input producer.

For `opcode_cpp`, `index` is a `LegalityQuery::Types` index, not a MachineInstr
operand index. Opcode names and indices must already be covered by the built-in
policy; this input supplies capabilities, not the semantics of a previously
unlisted opcode. Candidate order has no semantic effect: the generated policy
scans the complete set and chooses the narrowest type capable of carrying the
original width. Integer and floating-point candidates may coexist for an index
whose opcode supports both categories, notably the value/memory index of
`G_LOAD`/`G_STORE`.

An omitted opcode/type-index pair inherits the full matching `native_types`
list. A configured pair replaces that list. For example, with native integers
`[16, 32]`, constraining `G_MUL` type index 0 to `[16]` makes `G_MUL i16`
legal but not `G_MUL i32`; an `i8` multiply still widens to `i16`. Conversely,
constraining it to `[32]` widens both `i8` and `i16` multiplies to `i32`.
The policy never narrows an input merely because a smaller opcode-specific type
is available.

`G_BRCOND` is intentionally exempt from opcode-specific subsets: every integer
type in `native_types.integer_widths` is a legal condition register. Do not add
a `G_BRCOND` entry to `operation_type_constraints`; missing integer widths are
promoted against the complete native integer set.

Opcode-specific floating-point sets use the same promotion boundary as their
built-in opcode rule. Thus `G_FDIV` constrained to `[32]` promotes `f16` to
`f32`, while an unsupported `G_FMA` remains unsupported because `G_FMA` has no
built-in numeric-promotion action. Multi-type-index instructions can constrain
each index independently; for example, `G_SHL` uses index 0 for the shifted
value/result and index 1 for the shift amount. The complete input example gives
index 0 both `i16` and `i32` candidates while restricting index 1 to `i16`.

`G_FADD` additionally accepts an exact floating-point vector type directly.
This applies to both fixed and scalable vectors and is independent of the
scalar `native_types` and opcode-specific candidate lists. The template does
not widen vector elements, change their representation, or split the vector;
instruction selection must support every vector form that reaches this rule.

Plain non-atomic scalar `G_LOAD` and `G_STORE` use this same mechanism. Type
index 0 describes both the value type and, under this template's required
shape, the identical MMO memory type. Configure the two opcodes separately if
loads and stores have different capabilities, or give them identical lists if
the target uses one common memory capability set. Every configured width must
also be globally native. Exact pointer-valued and vector-valued loads and stores
remain directly legal regardless of the index-0 constraint; their address
operand is a pointer and is preserved. Vector types are not inferred from the
scalar candidate lists and are not transformed element-by-element. The
generated policy does not infer extending loads or truncating stores, apart
from the explicit one-byte scalar `i1` access expansion described below.

A floating-point load/store type absent from that opcode's
floating-point candidates may still use an equal-width integer representation
when the exact width occurs among the same opcode's integer candidates. The
legalizer bitcasts the value and changes the MMO type without changing its bit
width. For example, a `G_LOAD` constraint containing `integer_width: 16` but no
`floating_point_width: 16` accepts an `f16` load through an `i16` carrier. The
corresponding `G_STORE` behavior depends independently on its own constraint.

These templates assume that the ExtendedLLT reland is cherry-picked onto
`llvmorg-23-init` and enabled consistently by the integrating target. Integer,
floating-point, and generic `sN` types then have different identities. The
generated legalizer deliberately uses `LLT::integer(N)` and
`LLT::floatIEEE(N)`; do not replace them with the generic `LLT::scalar(N)`
wildcard.

For `intrinsic_id_cpp`, `index` is a zero-based explicit input argument index;
it excludes definitions and the intrinsic-ID operand. Integer candidates enable
the carrier conversions below. A floating-point candidate currently matches
only an actual argument with that exact native floating-point type; the
template does not infer numeric integer/float conversions. Like opcode entries,
intrinsic legalization treats the candidates as an unordered capability set.

Candidate selection is deterministic:

1. Preserve an exact actual type.
2. Prefer an equal-width floating-point-to-integer representation change.
3. For all remaining widening and constant-narrowing conversions, select the
   narrowest convertible candidate.

For example, actual `i32` with candidates `[i16, i32]` remains `i32`; actual
`f16` with `[i32, i16]` selects the equal-width `i16`; and actual `i8` with
the deliberately unsorted candidates `[i32, i16]` still selects `i16`.

For example, argument index `0` is mapped at legalization time to MIR operand
`MI.getNumExplicitDefs() + 1`. The runtime helper verifies that every listed
argument is a convertible virtual-register use before changing any operand.
Duplicate intrinsic IDs and duplicate argument indices must be rejected by the
input producer. Empty candidate lists and duplicate candidate types must also
be rejected. Intrinsics absent from `operation_type_constraints` pass through
unchanged.

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
- floating-point constants and operations: `G_FCONSTANT`; native scalar forms
  of `G_FADD`, `G_FSUB`, `G_FMUL`, `G_FDIV`, `G_FREM`, `G_FMA`,
  `G_FMAD`, `G_FNEG`, `G_FABS`, `G_FCANONICALIZE`, all standard
  `G_FMIN*`/`G_FMAX*` variants, `G_FSQRT`, `G_FCEIL`, `G_FFLOOR`, `G_FRINT`,
  `G_FNEARBYINT`, `G_FCOPYSIGN`; and wider-native promotion for unsupported
  scalar `G_FADD`, `G_FSUB`, `G_FMUL`, and `G_FDIV` types; exact
  floating-point vectors are also directly legal for `G_FADD`;
- scalar casts: supported integer `G_ANYEXT` pairs and `G_TRUNC`; `G_ZEXT` and
  `G_SEXT` on those same pairs are custom-normalized to `G_ANYEXT`; native
  `G_FPEXT`/`G_FPTRUNC` pairs, and `G_SITOFP`, `G_UITOFP`, `G_FPTOSI`,
  `G_FPTOUI` when their floating-point side is native;
- pointer representation: `G_PTR_ADD`, `G_INTTOPTR`, `G_PTRTOINT`; their
  integer operand or result follows the native integer-width policy;
- direct pointer carriers: `G_FRAME_INDEX`, `G_GLOBAL_VALUE`,
  `G_CONSTANT_POOL`, `G_BLOCK_ADDR`, `G_JUMP_TABLE`, `G_BRINDIRECT`;
- untyped unconditional control flow: `G_BR`;
- plain non-atomic scalar and exact vector memory: `G_LOAD`, `G_STORE`;
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
closed. `G_FCMP` promotes an unsupported floating-point input to its narrowest
wider native type, then gives the comparison result the exact same width as
that legalized input: `f16 -> i16`, `f32 -> i32`, and so on. The corresponding
exact integer type must be supported for `G_FCMP`; a merely wider integer does
not substitute for it. The condition input of `G_SELECT` uses the smallest
condition carrier. Pointer values are also accepted by `G_SELECT` and `G_PHI`.

An accepted scalar `G_SELECT` is not left for instruction selection. Custom
legalization splits its block into true, false, and merge blocks, emits
`G_BRCOND`/`G_BR`, and defines the original destination with a `G_PHI` in the
merge block. This applies to integer, floating-point, and pointer values and to
all selects, not only selects introduced by another generated rule. Instructions
after the select and the original CFG successors are moved to the merge block;
successor PHIs and the machine-function `NoPHIs` property are updated.

`G_FMAXIMUM` is custom-lowered through `G_FCMP` and `G_SELECT`, after promoting
an unsupported low floating-point type when a wider opcode type is available.
With `nnan nsz`, the expansion is the direct `ogt` compare/select form. Without
those flags it adds ordered-NaN and signed-zero repair selects so that NaNs
propagate and `+0.0` remains greater than `-0.0`, as required by
`llvm.maximum`. Signed-zero repair compares the equal-width integer bit
patterns with zero and therefore requires an integer carrier at least as wide
as the floating-point value. Every generated `G_SELECT` then takes the same CFG
path above, so neither `G_FMAXIMUM` nor `G_SELECT` reaches the selector.

The integer extension policy deliberately discards the high-bit guarantee of
`G_ZEXT` and `G_SEXT`: once their source/destination pair is supported, custom
legalization changes only the opcode to `G_ANYEXT`. This is appropriate only
when those high bits are unobservable, or when the target independently
guarantees the required register contents. It also applies to extensions
introduced by generic widening, not only to casts originating directly in IR.
LLVM may first fold an extension that participates in a legalization-artifact
chain; only an extension that survives those semantics-preserving combines
reaches this custom opcode rewrite.

When the target declares `ZeroOrOneBooleanContent`, legalization artifacts from
an original `icmp -> zext -> store` chain can be combined away after widening.
For an input and store carrier of `i16`, the legal form is one `G_ICMP i16`
followed by `G_STORE i16`; no mask with constant one is required. Other boolean
content conventions do not satisfy the generated boolean-memory contract.

`G_BRCOND` accepts every native integer condition directly. A condition whose
integer width is missing is promoted to the narrowest native integer at least
as wide as the original condition; the custom branch rewrite uses `G_ANYEXT`
for this carrier conversion. This covers both the usual promoted `i1`
condition and branches driven by an already-native wider integer. Non-integer
conditions and integer conditions wider than every native type fail closed.
Opcode-specific capability constraints do not restrict this rule.

When a missing-width condition is defined by `G_ICMP` or `G_FCMP` (possibly
through copies or integer casts), branch legalization predicts the compare's
eventual result carrier instead of selecting the smallest carrier in
isolation. For `G_FCMP`, that prediction first determines the legalized float
input and then uses its same-width integer type. This compensates for LLVM's
bottom-up legalization order. With an `i64` integer comparison, the artifact
combiner can therefore reduce the temporary `G_TRUNC`/boolean-extension chain
to `G_ICMP i64` followed directly by `G_BRCOND i64` rather than retaining an
`i64`-to-`i16` conversion; an `f32` comparison similarly reaches
`G_BRCOND i32`.

`G_PTR_ADD` preserves its pointer type. A missing integer offset width is
promoted to the narrowest native integer; the generic legalizer may first
create a signed extension, but this policy normalizes it to `G_ANYEXT`.
`G_INTTOPTR` promotion is normalized in the same way. `G_PTRTOINT` promotes a
missing integer result width and leaves a truncation for users of the original
result. Targets relying on signed pointer offsets or zero-filled pointer bits
must not use this relaxed extension policy without an equivalent target-side
guarantee.

The direct pointer-carrier group has no type mutation because pointer types are
not part of this compact native scalar input. Declaring these GMIR operations
legal only means that type legalization is complete. The target's instruction
selector must still implement its frame, relocation/code-model, constant-pool,
block-address, jump-table, and indirect-branch forms.

`G_BR` has no register type to legalize, so it is always legal at this layer.

`G_CONSTANT` uses the same integer-width policy: a native-width constant is
legal, while a missing integer width is promoted to the smallest wider native
integer. LLVM's generic legalizer handles the corresponding constant widening.

A native `G_FCONSTANT` is legal. An unsupported floating-point constant used
by a promoted numeric operation is folded with its generated extension into an
exactly extended native constant:

```text
%lo:fN = G_FCONSTANT value
%hi:fM = G_FPEXT %lo
  ->
%hi:fM = G_FCONSTANT fpext(value)
```

Here `fM` is the narrowest native IEEE type wider than `fN`. IEEE widening of
the constant is exact, and the unsupported `G_FCONSTANT` plus `G_FPEXT` are
removed. An unsupported constant with an available integer carrier can also be
folded directly into an integer constant:

```text
%dst:fN = G_FCONSTANT value
%bits:iN = G_BITCAST %dst
  ->
%bits:iN = G_CONSTANT bitcast(value)
```

The result registers and all their uses are preserved. Every non-debug use of
the unsupported constant must be either one of these generated native
`G_FPEXT`s or an equal-width integer carrier bitcast; mixed uses are supported.
The resulting `G_CONSTANT iN` is promoted by the integer-width policy when
`iN` itself is not native. Floating-point operations outside the promotion
list still fail closed for unsupported types; none are reinterpreted as
integer arithmetic.

Before its CFG expansion, `G_SELECT` may bitcast an unsupported floating-point
selected value to an equal-width integer and then promote that integer. This is
representation-safe for selection and is not applied to numerical
floating-point operations.
`G_PHI` promotes integer values but does not bitcast unsupported float values.

The templates also cover the artifacts introduced by this policy: integer
extensions to a native carrier are normalized to `G_ANYEXT`, while inverse
truncations and equal-width integer/floating-point bitcasts are legal. The
`G_FPEXT`/`G_FPTRUNC` pair between an unsupported low IEEE type and its selected
native computation type remains legal, as do the native floating-point
conversion pairs listed above.

A pre-isel generic opcode not listed by this generated policy is marked
`alwaysLegal()` and passes type legalization unchanged. An opcode that is
listed remains governed by its exact rules: a type combination that matches
neither a legality predicate nor a transformation is still rejected (or uses
an explicit `fallback()` where present). This default only skips legalization;
it does not imply that instruction selection or lowering supports the opcode.
Intrinsics absent from `operation_type_constraints` similarly pass unchanged,
while listed intrinsics must satisfy one candidate type rule for every
configured scalar argument.

Plain non-atomic `G_LOAD` and `G_STORE` require identical value and MMO memory
types. A scalar integer or floating-point operation is directly legal only when
its type is globally native and present in that opcode's index-0 type constraint
(or inherited from `native_types` when no constraint exists). Exact pointer and
vector operations are directly legal without a separate capability field. The
address pointer operand, alignment, ordering, and other MMO flags are preserved.
This vector rule accepts both fixed and scalable vectors as complete values; it
does not legalize their element types, split them, or use scalar carrier rules.

Scalar `i1` is the one built-in exception during legalization. Its ABI memory
object always occupies exactly one byte; booleans are not bit-packed. The
legalizer selects the narrowest byte-sized integer type available to both the
`G_LOAD` and `G_STORE` index-0 constraints. It also requires legalizable
`G_AND` and, for an access wider than one byte, `G_OR` carriers. With an `i8`
access type, the value and MMO are changed to `i8` and a mask canonicalizes the
stored byte to zero or one:

```text
G_LOAD  i1, memory i1  -> G_LOAD i8; G_AND 1; G_TRUNC to i1
G_STORE i1, memory i1  -> G_ANYEXT to i8; G_AND 1; G_STORE i8
```

When the common access type is wider, the address and byte layout do not
change. A load reads the wider integer and masks the boolean bit from the first
addressed byte. A store performs a non-atomic read/modify/write: it loads the
wider integer, clears the first addressed byte, inserts a canonical zero or
one, and stores the merged value. This compact rule supports little-endian
targets only; a big-endian DataLayout is rejected. For an `i16` access the
store is equivalent to:

```text
old    = G_LOAD i16, memory i16
bool   = G_AND (G_ANYEXT i1), 0x0001
keep   = G_AND old, 0xff00
merged = G_OR keep, bool
G_STORE merged, memory i16
```

This expansion assumes a widened access starting at any valid boolean byte
address is permitted with the original alignment and may touch the adjacent
bytes. The store preserves those bytes, but the read/modify/write is not safe
for atomic or volatile memory and is not a synchronization mechanism. Metadata
that described only the original byte is not copied to widened accesses. The
generated load records its masked zero-extension fact with `G_ASSERT_ZEXT`.
`ZeroOrOneBooleanContent` remains the boolean-carrier contract, while the
explicit store mask makes the in-memory byte canonical. Atomic, volatile, and
multi-MMO boolean operations are not rewritten. Vector boolean memory follows
the exact-vector rule and does not use this scalar byte rewrite.

When a floating-point type is not directly supported but the exact same-width
integer is present in that opcode's index-0 constraint, the representation is
changed without changing the access width. LLVM clears load range metadata
because the old floating-point range is no longer valid:

```text
G_LOAD  fN              -> G_LOAD iN; G_BITCAST iN to fN
G_STORE fN              -> G_BITCAST fN to iN; G_STORE iN
```

Here `fN` and `iN` have identical bit widths. Except for the scalar `i1` byte
access expansion above, there is deliberately no integer widening rule for
memory operations: an unsupported `G_LOAD/G_STORE i8` does
not become a mismatched `value i16, memory i8` operation. When `fN` is also not
a native floating-point register type, a stored `G_FCONSTANT` can subsequently
fold with its generated bitcast into a `G_CONSTANT`, using the same
constant-carrier rule described above.

This built-in rule is deliberately limited to one-MMO, non-atomic operations
with exact value/memory type identity. Scalar values use the configured
capabilities and carrier rules, while vector values pass unchanged. Atomic,
indexed, multi-MMO, and mismatched value/memory operations fall back to the
target's other rules rather than being inferred from register types.

The audit deliberately does not mark `G_ADDRSPACE_CAST`, `G_PTRMASK`,
`G_DYN_STACKALLOC`, `G_STACKSAVE`, `G_STACKRESTORE`, `G_BRJT`, atomic memory
operations, or `G_VAARG`/`G_VASTART` directly legal. Those require address-space,
pointer-width, stack/ABI, jump-table, atomic, or varargs policy that cannot be
derived from `native_types`. Optimization hints such as `G_ASSERT_ZEXT`,
`G_ASSERT_SEXT`, and `G_ASSERT_ALIGN` are outside the normal pre-isel generic
opcode legality range and are eliminated later; they need no generated rule.

At `llvmorg-23-init`, the explicitly configured pointer and memory rules that
end in `fallback()` can still query the embedded `LegacyLegalizerInfo`. The
generated constructor therefore finishes with
`getLegacyLegalizerInfo().computeTables()`. This is required even when the
target adds no legacy actions; omitting it triggers the `TablesInitialized`
assertion on the first such query.

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

1. Validate the target spelling; exactly one operation identifier per entry;
   duplicate opcode/intrinsic IDs and indices; non-empty `scalar_types` and
   candidate lists; duplicate candidates; exactly one type variant per
   candidate; that every candidate type is native; and that floating widths are
   drawn from `16`, `32`, `64`, and `128`.
2. Disable HTML escaping before rendering C++ values.
3. Run `clang-format` on the generated source.
4. Compile against the exact LLVM payload.
5. Confirm that target TableGen uses `-gisel-extended-llt` and target runtime
   setup calls `LLT::setUseExtended(true)` before GlobalISel creates LLTs.
6. Test every native integer, every gap below the largest integer, native and
   unsupported floating-point types, and a width above the largest integer.
7. Inspect post-legalization MIR and run instruction selection for every
   generated artifact, intrinsic, comparison, branch, and `G_PHI`; also verify
   that `G_FMAXIMUM` and `G_SELECT` have been eliminated.
8. Check that one unlisted pre-isel generic opcode and one unlisted intrinsic
   pass legalization unchanged, then independently confirm that the target can
   select, lower, or eliminate them.
