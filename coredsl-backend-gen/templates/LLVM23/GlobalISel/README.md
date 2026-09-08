# LLVM 23 GlobalISel legalizer templates

These templates generate a target-specific `LegalizerInfo` for LLVM 23 with
ExtendedLLT enabled. They contain scalar type policy, selected instruction
expansions, and local integer carrier coalescing.

The main backend emitter in `lib/Emit/LLVM23/LLVM23Emitter.cpp` has a separate,
simplified legalizer output path. Using that emitter does not automatically
integrate these templates. See [TARGET_INTEGRATION.md](TARGET_INTEGRATION.md)
for the integration steps.

## Input and generation

The input has three fields:

| Field | Meaning |
| --- | --- |
| `target` | Target spelling used for the class, filenames, intrinsic header, and command-line option. |
| `native_types` | Native integer and IEEE floating-point carrier widths. |
| `operation_type_constraints` | Scalar candidates grouped by opcode/type index or intrinsic/input argument index. |

[legalizer-input.example.json](legalizer-input.example.json) is a complete
example. [llvm-api-render-example.cpp](llvm-api-render-example.cpp) constructs
the same input with LLVM JSON APIs and renders it through
`llvm::mustache::Template`. It also accepts a JSON file as its second argument.

```sh
llvm-api-render-example LegalizerInfo.h.mustache input.json > ExampleLegalizerInfo.h
llvm-api-render-example LegalizerInfo.cpp.mustache input.json > ExampleLegalizerInfo.cpp
```

The renderer disables HTML escaping and derives the `target_upper` include-guard
lambda from `target`. The generated constructor takes no arguments:

```cpp
Legalizer = std::make_unique<ExampleLegalizerInfo>();
```

Integer types use `LLT::integer(N)`; IEEE floats use `LLT::floatIEEE(N)`.
Generic `LLT::scalar(N)` is a different ExtendedLLT identity. Both target
TableGen and target runtime must enable ExtendedLLT.

Input producers must validate:

- a valid target identifier and trusted C++ opcode/intrinsic identifiers;
- non-empty, unique, positive native integer widths;
- unique native IEEE widths from `16, 32, 64, 128` (the float list may be empty);
- exactly one of `opcode_cpp` or `intrinsic_id_cpp` per operation;
- unique operation IDs, unique indices, non-empty candidate lists, and exactly
  one type variant per candidate;
- every operation candidate belongs to its corresponding native type list;
- opcode indices describe configurable scalar indices listed below.

An opcode entry's `index` is a `LegalityQuery::Types` index. An intrinsic
entry's `index` is a zero-based explicit input argument index, excluding
definitions and the intrinsic-ID operand. These are not interchangeable.

The generated constructor rejects opcode constraints for removed/unhandled
operations, fixed artifacts, or indices without a configurable scalar rule.
It also checks native membership. These checks supplement input-producer
validation; they do not make arbitrary C++ strings safe renderer input.

## Three rule domains

The domains are deliberately separate:

| Domain | Type policy | Effect of `-Example-use-legalizer=true` |
| --- | --- | --- |
| Configurable scalar operations | Opcode/type-index candidates, including constants and both compare indices. | Missing pairs have no carrier. |
| Fixed cast artifacts | Native-carrier extension/truncation and representation-bitcast contracts. | Unchanged; these are built-in rules, not missing-pair fallback. |
| Intentional pass-through | Unlisted generic opcodes, unlisted intrinsics, unconditional branch, and explicitly accepted pointer/vector value forms. | Unchanged; instruction selection remains the target's responsibility. |

Do not supply scalar constraints for fixed artifacts or unlisted opcodes and
expect them to change those domains.

For configurable scalar operations, `getOpcodeTypeForWidth` is the only native
fallback decision:

1. If the opcode/type-index pair exists, use only its candidates. An unavailable
   width or scalar kind does not fall back.
2. If the pair is absent and `-Example-use-legalizer=false` (the default), use
   the matching `native_types` list.
3. If the pair is absent and the option is true, return no carrier.

The process-wide option name preserves the exact target spelling. It controls
native scalar fallback, not whether LLVM runs its Legalizer pass.

An integer missing width chooses the smallest supported width at least as large
as the original. Candidate ordering has no effect. Ordinary nonconstant integer
values are never narrowed or split by this policy. Opcode candidates do not
encode arbitrary correlations between operands; comparisons explicitly enforce
the shared-width relationship described below.

## Configurable scalar rules

| Operations | Scalar indices and action |
| --- | --- |
| `G_IMPLICIT_DEF, G_FREEZE, G_CONSTANT_FOLD_BARRIER` | Index 0: legal native candidates; integer promotion. Pointer values pass directly. |
| `G_CONSTANT` | Integer index 0: exact candidate or integer promotion. Both paths use the same query. Pointer constants pass directly. |
| `G_ADD, G_SUB, G_MUL, G_SDIV, G_UDIV, G_SREM, G_AND, G_OR, G_XOR, G_SMIN, G_SMAX, G_UMIN, G_UMAX, G_ABS, G_SEXT_INREG, G_BSWAP, G_BITREVERSE` | Integer index 0: exact candidate or integer promotion. |
| `G_UREM` | Same integer carrier policy, then the guarded value-dependent rewrite below. |
| `G_SHL, G_LSHR, G_ASHR` | Integer indices 0 and 1 are independently constrained. |
| `G_CTLZ, G_CTLZ_ZERO_UNDEF, G_CTTZ, G_CTTZ_ZERO_UNDEF, G_CTPOP` | Integer result/input indices 0 and 1 are independently constrained. |
| `G_FCONSTANT` | Float index 0: exact candidate; unsupported constants may fold through their generated uses. |
| `G_FADD, G_FSUB, G_FMUL, G_FDIV, G_FNEG, G_FSQRT, G_FEXP` | Float index 0: exact candidate or promotion to the smallest wider IEEE candidate. |
| `G_FREM, G_FMA, G_FMAD, G_FABS, G_FCANONICALIZE`, standard `G_FMIN*/G_FMAX*`, `G_FCEIL, G_FFLOOR, G_FRINT, G_FNEARBYINT` | Float index 0: exact candidate only. |
| `G_FCOPYSIGN` | Float indices 0 and 1. |
| `G_SITOFP, G_UITOFP` | Float result index 0, integer input index 1; integer-side promotion. |
| `G_FPTOSI, G_FPTOUI` | Integer result index 0, float input index 1; integer-side promotion. |
| `G_ICMP, G_FCMP` | Result index 0 and input index 1 participate in a common-width choice. |
| `G_BRCOND` | Integer condition index 0, with comparison-aware promotion. |
| `G_SELECT` | Value index 0 and integer condition index 1, then CFG expansion. |
| `G_PHI` | Value index 0; integer promotion, exact float candidates, or direct pointer values. |
| `G_LOAD, G_STORE` | Scalar value/MMO index 0; the address is not a configurable scalar index. |

`G_FMAXIMUM` follows the ordinary exact-type floating rule, just like
`G_FMINIMUM`. It is not custom-expanded to compare/select/CFG, and unsupported
widths do not receive a special promotion rule. The target selector or lowering
must implement the operation's semantics.

`G_FADD` additionally accepts complete fixed/scalable floating-point vectors.
There is no general vector element promotion or vector splitting policy.
Numeric floating operations are never replaced by integer arithmetic solely
because a float carrier is missing.

## Comparisons and local carrier coalescing

`G_ICMP` uses an integer carrier in the intersection of its result and input
candidate sets. Exact legality, the default promotion, branch prediction, and
local promotion all respect both sets. An input `i64` does not authorize an
`i64` result if index 0 excludes it. Pointer comparisons have no rule here.

`G_FCMP` uses a supported float input and an integer result of exactly the same
width. It searches both sets together: result `[i64]` and input `[f32, f64]`
select `(i64, f64)`, including for a narrower original float input.

`G_BRCOND` has its own index-0 candidates. When widening a condition from an
`ICMP/FCMP`, it predicts the comparison carrier and uses it if the branch
supports it. Otherwise it chooses its own smallest wider candidate.

Ordinary integer producers choose their smallest supported carrier first.
If a missing-width result has one same-block use through `G_ANYEXT`, they may
adopt that wider carrier when the producer supports it. The search may pass
through up to eight same-type `COPY` instructions. Multiple uses, cross-block
uses, PHIs, and raw defined extensions stop this preference propagation.

An `ICMP` with a still-missing-width result can adopt a branch's wider carrier
for both input and result. For example, with `ICMP [i16, i32]` and
`BRCOND [i32]`, `ICMP (i1, i8) -> BRCOND i1` can become
`ICMP (i32, i32) -> BRCOND i32`. LLVM's artifact combiner removes the temporary
truncation/extension bridge. This is a bounded local optimization, not global
type inference. Defined input extensions required by comparisons remain
semantic boundaries while present.

## Cast artifacts and constant folding

The fixed artifact rules accept:

- integer extension to a native integer and truncation from a native integer;
- IEEE float extension to a native float and truncation from a native float;
- equal-width integer/float representation bitcasts with an available integer
  carrier.

Supported scalar `G_ZEXT/G_SEXT` pairs are custom-normalized to `G_ANYEXT`.
This retains the target's existing low-bit payload convention; it is not a
general equivalence for arbitrary LLVM integer extensions. Pointer-specific
extension exceptions have been removed together with pointer legalization.

Unsupported floating constants are rewritten only when every non-debug use
can be folded: an integer bitcast becomes a bit-identical `G_CONSTANT`, or an
extension becomes an exactly extended, operation-supported `G_FCONSTANT`.
Mixed supported uses work; arbitrary additional uses reject the rewrite.
The resulting constants must themselves have legalizable opcode carriers,
including in strict mode.

## Memory and instruction expansions

Plain load/store rules require one non-atomic MMO and identical value/MMO
types. Scalar integer/float forms use opcode index 0. Unsupported float memory
can bitcast to an exactly equal-width integer accepted by that memory opcode.
General integer memory widening, extending loads, and truncating stores are
not inferred. Complete fixed/scalable vectors and pointer-valued accesses are
accepted directly.

Scalar `i1` is a one-byte object. Its expansion selects the smallest byte-sized
native access carrier supported by both load and store, with legalizable
`AND`, constants, and (for wider accesses) `OR`. It is limited to little-endian,
non-atomic, non-volatile accesses. Wider loads mask the boolean bit; wider stores
read/modify/write the access unit to preserve neighboring bytes. The target
must permit that widened access at the original alignment. This template does
not prove adjacent-byte accessibility.

Scalar `G_SELECT` expands to a branch diamond and merge `G_PHI`. Before
modifying the CFG it checks carriers for the emitted branch and PHI. An integer
PHI may require further promotion; float PHIs require an exact candidate.
Original SSA destination and successor PHIs are preserved.

`G_UREM x,y` becomes `G_AND x,y-1` only when `y` is known to be a power of
two and the replacement `AND` and constants have carriers. A dynamic mask also
requires same-width `G_ADD`. Zero, non-power-of-two, unknown divisors, or missing
replacement capabilities leave the supported `UREM` unchanged.

## Intrinsic input adaptation

For each configured scalar input:

1. Preserve an exact type.
2. Prefer an equal-width floating-to-integer bit representation.
3. Otherwise choose the smallest convertible candidate, independent of order.

Float candidates require an exact actual float type. Integer candidates accept
integer widening or float bit representation followed by integer widening.
Float constants use their exact `APFloat` bit pattern directly. Integer
constants may narrow only when the value fits the destination signed or
unsigned range; nonconstant integers cannot narrow. Required constant
materialization is checked before changing arguments.

All configured arguments are validated before mutation. Intrinsic result
adaptation, numeric float-to-integer conversion, and generic software floating
point are outside this policy. Unlisted intrinsics pass through.

## Removed pointer policy and testing

There are no dedicated rules for `G_PTR_ADD, G_INTTOPTR, G_PTRTOINT, G_PTRMASK`,
address formation, or pointer comparisons. Removed generic pointer opcodes
fall under the intentional unlisted-opcode pass-through; no pointer width
normalization is performed. Scalar `ICMP` rejects pointer inputs. Pointer
address operands and pointer-valued memory/PHI/select/constant forms remain
usable. The legalizer no longer stores or takes a `DataLayout`; `i1` memory
checks the function's layout through `MachineIRBuilder`.

The source is organized into capability lookup, type predicates/artifacts,
local carrier coalescing, instruction rewrites, intrinsic adaptation, and rule
registration/dispatch. The scalar query owns fallback; registration separately
records which scalar indices are configurable.

CTest compiles the rendered templates and runs configured/missing-constraint
fixtures with the default option, explicit false, and true. Query checks cover
both scalar kinds, constants, compare constraints, FMAXIMUM, removed pointer
rules, casts, memory, and SELECT dependencies. When the native target's CodeGen
library is available, the same tests also exercise complete MIR legalization,
local coalescing and its boundaries, constant folding, CFG expansion, i1
read/modify/write, and UREM dependency guards.

These tests validate the generated policy. They do not establish another
target's instruction-selector coverage or physical memory-access guarantees.
