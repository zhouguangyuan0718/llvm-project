# Integrating the generated legalizer

The target must already provide GlobalISel CallLowering, RegisterBankInfo, and
InstructionSelector components. This guide integrates the standalone templates,
not the separate simplified policy emitted by LLVM23Emitter.cpp.

## 1. Supply capability data and render

Use the input shape in [legalizer-input.example.json](legalizer-input.example.json).
Set `target` to the exact LLVM target spelling, for example `Toy16`, and collect
scalar register carriers from target instruction results and operands:

```cpp
Root["target"] = "Toy16";
// native_types: integer_widths [16,32], floating_point_widths [32]
// operation_type_constraints: generic opcode/type indices and intrinsic inputs
```

Do not collect immediate widths, memory-only widths, or IR source types that
must first be converted into register carriers. Each operation candidate must
belong to the appropriate native list. For opcode indices and fixed artifact
exclusions, follow the [rule domains and operation table](README.md).

Register `target_upper` and disable HTML escaping as in
`llvm-api-render-example.cpp`. Render and format:

```text
LegalizerInfo.h.mustache   -> GISel/Toy16LegalizerInfo.h
LegalizerInfo.cpp.mustache -> GISel/Toy16LegalizerInfo.cpp
```

The generated class is `llvm::Toy16LegalizerInfo`. If the input contains
intrinsics, the source includes `llvm/IR/IntrinsicsToy16.h`; its enum names must
match those emitted from the target intrinsic TableGen definitions. Inputs
without intrinsics do not need that header.

## 2. Enable ExtendedLLT

This policy requires LLVM 23 with the ExtendedLLT implementation used by this
repository. Enable it both in TableGen and before creating runtime LLTs:

```cmake
tablegen(LLVM Toy16GenGlobalISel.inc
  -gen-global-isel
  -gisel-extended-llt)
```

```cpp
#include "llvm/CodeGenTypes/LowLevelType.h"

// In the target-machine initialization path, before GlobalISel creates types:
LLT::setUseExtended(true);
```

Use `LLT::integer` and `LLT::floatIEEE` consistently in target code and tests.
Generic `LLT::scalar` does not match these exact identities.

## 3. Compile and expose the class

Add `GISel/Toy16LegalizerInfo.cpp` to the target's CodeGen library. Expose its
instance through the subtarget:

```cpp
// Toy16Subtarget.h
std::unique_ptr<LegalizerInfo> Legalizer;
const LegalizerInfo *getLegalizerInfo() const override;

// Toy16Subtarget.cpp
#include "GISel/Toy16LegalizerInfo.h"

// In the subtarget constructor:
Legalizer = std::make_unique<Toy16LegalizerInfo>();

const LegalizerInfo *Toy16Subtarget::getLegalizerInfo() const {
  return Legalizer.get();
}
```

The constructor takes neither Subtarget nor DataLayout. Integrations using the
previous `Toy16LegalizerInfo(TM.createDataLayout())` call must update it.

Keep the standard GlobalISel pipeline: IRTranslator, Legalizer, RegBankSelect,
InstructionSelect, plus normal target/pass registration. This template does
not install those components.

## 4. Choose the fallback mode

The generated source registers `-Toy16-use-legalizer`, default false.

| Configuration | Result |
| --- | --- |
| Explicit opcode/type-index entry | Always use only that entry. |
| Missing scalar pair, option false | Native scalar fallback permitted. |
| Missing scalar pair, option true | No scalar carrier. |
| Fixed cast artifacts or intentional pass-through | Their built-in policy applies in both modes. |

Strict mode requires explicit entries for scalar operations that legalization
can produce, including `G_CONSTANT`, `G_FCONSTANT`, and `G_IMPLICIT_DEF`
when used. Both compare indices require compatible carriers.

For example:

- scalar SELECT expansion needs supported BRCOND and PHI carriers;
- i1 memory expansion needs load/store, AND, constants, and possibly OR;
- constant UREM masking needs AND and constants; a dynamic mask also needs ADD;
- intrinsic argument conversion may need operation-supported constants.

Do not configure scalar entries for artifact-only instructions
(`ANYEXT/ZEXT/SEXT/TRUNC/BITCAST/FPEXT/FPTRUNC`), unconditional BR, or removed
pointer opcodes. Their type indices are not scalar capability inputs.

Pointer arithmetic, conversions, address formation, and pointer-comparison
legalization must be handled by the integrating target when needed. Removed
unlisted pointer opcodes pass through this template; pointer ICMP is rejected
by the scalar comparison rule. No pointer/index width is normalized here.

FMAXIMUM now uses the ordinary exact-type floating rule. If it reaches the
selector, the target must implement its semantics. There is no generated
compare/select expansion for it.

## 5. Verify the target integration

Build the target and stop after legalization in both modes:

```sh
cmake --build <llvm-build> --target LLVMToy16CodeGen llc
<llvm-build>/bin/llc -mtriple=toy16 -global-isel \
  -Toy16-use-legalizer=false -verify-machineinstrs \
  -stop-after=legalizer input.ll -o -
<llvm-build>/bin/llc -mtriple=toy16 -global-isel \
  -Toy16-use-legalizer=true -verify-machineinstrs \
  -stop-after=legalizer input.ll -o -
```

Test native widths, gaps, and values wider than the largest carrier. Exercise
different result/input constraints for ICMP and FCMP, and verify that unsupported
result types cannot become directly legal merely because the inputs are legal.

Inspect single-use integer producer/consumer chains and ICMP-to-BRCOND chains.
Check that redundant truncation/extension bridges disappear, while multiple
uses, cross-block uses, and raw defined extensions retain their boundaries.

Check constant folding, intrinsic bit patterns and representable narrowing,
exact memory MMOs, i1 load/store expansions, scalar SELECT CFGs, and guarded
constant/dynamic UREM cases. For wider i1 access units, independently establish
the target's alignment and adjacent-byte access contract.

Finally run instruction selection:

```sh
<llvm-build>/bin/llc -mtriple=toy16 -global-isel \
  -Toy16-use-legalizer=true -verify-machineinstrs input.ll -o /dev/null
```

The selector must cover the remaining scalar/vector forms, FMAXIMUM, comparison
and branch carriers, PHIs, constants, and cast artifacts. Unlisted-opcode
pass-through is not evidence that an instruction is selectable.
