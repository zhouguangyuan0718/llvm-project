# LLVM 23 GlobalISel legalizer templates

This package generates one target-specific `LegalizerInfo` class. The renderer
input contains one target name, target-native scalar types, and intrinsic scalar
argument indices. File names, the class name, the include guard, the `llvm`
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
  "intrinsics": [
    {
      "id_cpp": "Intrinsic::example_f16_op",
      "scalar_argument_indices": [0]
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

These templates assume that the ExtendedLLT reland is cherry-picked onto
`llvmorg-23-init` and enabled consistently by the integrating target. Integer,
floating-point, and generic `sN` types then have different identities. The
generated legalizer deliberately uses `LLT::integer(N)` and
`LLT::floatIEEE(N)`; do not replace them with the generic `LLT::scalar(N)`
wildcard.

Each intrinsic entry contains only:

- `id_cpp`: the C++ intrinsic ID used in the generated `switch`;
- `scalar_argument_indices`: zero-based explicit input argument indices,
  excluding definitions and the intrinsic-ID operand.

For example, argument index `0` is mapped at legalization time to MIR operand
`MI.getNumExplicitDefs() + 1`. The runtime helper verifies that every listed
argument is a virtual-register use with an integer or floating-point scalar
type before changing the instruction. Duplicate intrinsic IDs and duplicate
argument indices must be rejected by the input producer. Unknown intrinsics
return `false`.

For a listed intrinsic argument the generated boundary is:

```text
nonconstant fN -> G_BITCAST iN  -> G_ANYEXT iM -> intrinsic
G_FCONSTANT fN -> G_CONSTANT iN -> G_ANYEXT iM -> intrinsic
iN -----------------------------> G_ANYEXT iM -> intrinsic
```

The extension is omitted when `iN` is native. Thus a half constant passed to an
intrinsic on an `i16` target becomes one `G_CONSTANT i16` whose value is the
exact `APFloat::bitcastToAPInt()` result; no `G_BITCAST` is emitted. The original
`G_FCONSTANT` is removed by normal dead-instruction handling when it has no
other non-debug users. `G_ANYEXT` preserves only a low-bit payload convention;
a numerically signed or unsigned argument needs a different semantic rule.

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

The smallest native integer is the condition carrier. `G_ICMP` promotes both
its result and integer input type; pointer inputs are accepted without changing
their pointer type. `G_FCMP` promotes only its result and accepts only native
floating-point inputs. `G_BRCOND` and the condition input of `G_SELECT` use the
same carrier. Pointer values are also accepted by `G_SELECT` and `G_PHI`.

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

Plain non-atomic scalar `G_LOAD` and `G_STORE` apply the same value-carrier
policy while preserving their pointer operand and MMO width, alignment, and
flags. LLVM clears load range metadata when the loaded representation type is
changed because the old range is no longer valid:

```text
G_LOAD  fN              -> G_LOAD iN; G_BITCAST iN to fN
G_STORE fN              -> G_BITCAST fN to iN; G_STORE iN
G_LOAD  iN, memory iK   -> G_LOAD iM, memory iK; G_TRUNC iM to iN
G_STORE iN, memory iK   -> G_ZEXT/G_ANYEXT iN to iM; G_STORE iM, memory iK
```

Here `fN` is an unsupported floating-point type, `iM` is the narrowest native
integer carrier for `iN`, and `K <= N <= M`. The equal-width float/integer
conversion changes the MMO type but not its size. Integer promotion therefore
produces an any-extending load or truncating store without widening the actual
memory access. Store promotion uses `G_ZEXT` for `i1` and `G_ANYEXT` otherwise.
A stored `G_FCONSTANT` can subsequently fold with its generated bitcast into a
`G_CONSTANT`, using the same constant-carrier rule described above.

This built-in rule is deliberately limited to one-MMO, non-atomic scalar
operations whose value and memory types are compatible. Native floating-point
loads and stores require an exact value/memory type match. Atomic, indexed,
vector, multi-MMO, and mismatched floating-point memory operations fall back to
the target's other rules rather than being inferred from scalar register types.
Exact pointer-valued loads and stores are accepted without changing the pointer
or memory type.

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
   the strictly increasing integer widths, and the strictly increasing IEEE
   floating-point widths drawn from `16`, `32`, `64`, and `128`.
2. Disable HTML escaping before rendering C++ values.
3. Run `clang-format` on the generated source.
4. Compile against the exact LLVM payload.
5. Confirm that target TableGen uses `-gisel-extended-llt` and target runtime
   setup calls `LLT::setUseExtended(true)` before GlobalISel creates LLTs.
6. Test every native integer, every gap below the largest integer, native and
   unsupported floating-point types, and a width above the largest integer.
7. Inspect post-legalization MIR and run instruction selection for every
   generated artifact, intrinsic, comparison, and consumer.
