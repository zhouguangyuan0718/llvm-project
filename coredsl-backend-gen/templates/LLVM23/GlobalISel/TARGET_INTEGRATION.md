# Integrating the generated legalizer into another LLVM target

This example integrates the templates into a fictional LLVM target named
`Toy16`. It assumes that the target already has, or is also adding, the other
required GlobalISel components: `CallLowering`, `RegisterBankInfo`, and an
`InstructionSelector`.

## 1. Construct the renderer input

The generator supplies only the target name, native scalar types, and one
operation type-constraint table shared by generic opcodes and intrinsics:

```cpp
llvm::json::Object Root;
Root["target"] = "Toy16";

llvm::json::Object NativeTypes;
NativeTypes["integer_widths"] = llvm::json::Array{16, 32};
NativeTypes["floating_point_widths"] = llvm::json::Array{32};
Root["native_types"] = std::move(NativeTypes);

auto IntegerType = [](unsigned Width) {
  llvm::json::Object Type;
  Type["integer_width"] = Width;
  return Type;
};
auto FloatType = [](unsigned Width) {
  llvm::json::Object Type;
  Type["floating_point_width"] = Width;
  return Type;
};
auto MakeScalarType = [](unsigned Index, llvm::json::Array Types) {
  llvm::json::Object ScalarType;
  ScalarType["index"] = Index;
  ScalarType["types"] = std::move(Types);
  return ScalarType;
};

auto MakeOperation = [](llvm::StringRef IdKey, llvm::StringRef Id,
                        llvm::json::Array ScalarTypes) {
  llvm::json::Object Operation;
  Operation[IdKey] = Id;
  Operation["scalar_types"] = std::move(ScalarTypes);
  return Operation;
};
auto MemoryTypes = [&]() {
  llvm::json::Array Types;
  Types.emplace_back(IntegerType(16));
  Types.emplace_back(IntegerType(32));
  return Types;
};

llvm::json::Array MulTypes;
MulTypes.emplace_back(IntegerType(16));

llvm::json::Array FDivTypes;
FDivTypes.emplace_back(FloatType(32));

llvm::json::Array ShlValueTypes;
ShlValueTypes.emplace_back(IntegerType(32));
ShlValueTypes.emplace_back(IntegerType(16));

llvm::json::Array ShlAmountTypes;
ShlAmountTypes.emplace_back(IntegerType(16));

llvm::json::Array IntrinsicTypes;
IntrinsicTypes.emplace_back(IntegerType(32));
IntrinsicTypes.emplace_back(IntegerType(16));

auto SingleScalarType = [&](unsigned Index, llvm::json::Array Types) {
  llvm::json::Array ScalarTypes;
  ScalarTypes.emplace_back(MakeScalarType(Index, std::move(Types)));
  return ScalarTypes;
};

llvm::json::Array ShlScalarTypes;
ShlScalarTypes.emplace_back(
    MakeScalarType(0, std::move(ShlValueTypes)));
ShlScalarTypes.emplace_back(
    MakeScalarType(1, std::move(ShlAmountTypes)));

Root["operation_type_constraints"] = llvm::json::Array{
    llvm::json::Value(MakeOperation(
        "opcode_cpp", "G_LOAD", SingleScalarType(0, MemoryTypes()))),
    llvm::json::Value(MakeOperation(
        "opcode_cpp", "G_STORE", SingleScalarType(0, MemoryTypes()))),
    llvm::json::Value(MakeOperation(
        "opcode_cpp", "G_MUL", SingleScalarType(0, std::move(MulTypes)))),
    llvm::json::Value(MakeOperation(
        "opcode_cpp", "G_FDIV", SingleScalarType(0, std::move(FDivTypes)))),
    llvm::json::Value(MakeOperation(
        "opcode_cpp", "G_SHL", std::move(ShlScalarTypes))),
    llvm::json::Value(MakeOperation(
        "intrinsic_id_cpp", "Intrinsic::toy16_f16_op",
        SingleScalarType(0, std::move(IntrinsicTypes))))};
```

The `G_SHL` index-0 and intrinsic candidate lists deliberately spell `i32`
before `i16`. Capability-list order has no semantic effect; legalization still
selects the narrowest convertible type after preserving an exact match.

Register the `target_upper` lambda as shown in
`llvm-api-render-example.cpp`, disable HTML escaping, and render:

```text
LegalizerInfo.h.mustache
  -> llvm/lib/Target/Toy16/GISel/Toy16LegalizerInfo.h

LegalizerInfo.cpp.mustache
  -> llvm/lib/Target/Toy16/GISel/Toy16LegalizerInfo.cpp
```

For `target = "Toy16"`, the generated header declares
`llvm::Toy16LegalizerInfo`, and its guard is
`LLVM_LIB_TARGET_TOY16_LEGALIZERINFO_H`.

The source includes `llvm/IR/IntrinsicsToy16.h`. If the target uses the custom
intrinsic in this example, add its `.td` file to `llvm/IR/Intrinsics.td` and add
the corresponding `-gen-intrinsic-enums` entry to
`llvm/include/llvm/IR/CMakeLists.txt`, following the existing target intrinsic
headers. The `intrinsic_id_cpp` spelling must match the generated enum.

## 2. Enable ExtendedLLT consistently

This template targets `llvmorg-23-init` plus the ExtendedLLT reland. Enable
ExtendedLLT in both the generated matcher and the target runtime.

Add `-gisel-extended-llt` to the target's GlobalISel TableGen command:

```cmake
tablegen(LLVM Toy16GenGlobalISel.inc
  -gen-global-isel
  -gisel-extended-llt)
```

Enable the same representation in the target-machine constructor, before
GlobalISel creates any LLTs:

```cpp
#include "llvm/CodeGenTypes/LowLevelType.h"

Toy16TargetMachine::Toy16TargetMachine(/* existing arguments */)
    : LLVMTargetMachine(/* existing initialization */) {
  LLT::setUseExtended(true);
  // Existing target-machine initialization...
}
```

The switch is process-wide in this LLVM revision. Setting only the TableGen
flag or only the runtime flag is invalid: generated `GIM_SwitchType` tables use
the exact ExtendedLLT raw identity, so `s32`, `i32`, and `f32` are different
keys.

## 3. Compile the generated source

Add the generated implementation to `llvm/lib/Target/Toy16/CMakeLists.txt`:

```cmake
add_llvm_target(Toy16CodeGen
  GISel/Toy16CallLowering.cpp
  GISel/Toy16InstructionSelector.cpp
  GISel/Toy16LegalizerInfo.cpp
  GISel/Toy16RegisterBankInfo.cpp
  Toy16Subtarget.cpp
  Toy16TargetMachine.cpp
  # Other target sources...
)
```

The generated legalizer has no Subtarget constructor argument. CPU or feature
dependent legality is intentionally outside this simplified policy.

The generated constructor also finalizes the embedded legacy tables required
by `llvmorg-23-init`. Do not remove its
`getLegacyLegalizerInfo().computeTables()` call: explicitly configured pointer
and memory rules ending in `fallback()` can still reach the legacy legalizer,
even if the target defines no legacy actions itself. Completely unlisted
pre-isel generic opcodes instead pass type legalization unchanged.

## 4. Expose it from the Subtarget

The GlobalISel `Legalizer` pass obtains the policy through
`TargetSubtargetInfo::getLegalizerInfo()`. Add storage and the override to
`Toy16Subtarget.h`:

```cpp
#include "llvm/CodeGen/GlobalISel/LegalizerInfo.h"

#include <memory>

namespace llvm {

class Toy16Subtarget final : public Toy16GenSubtargetInfo {
  // Existing CallLowering, selector, and register-bank members...
  std::unique_ptr<LegalizerInfo> Legalizer;

public:
  Toy16Subtarget(const Triple &TT, StringRef CPU, StringRef FS,
                 const TargetMachine &TM);

  const LegalizerInfo *getLegalizerInfo() const override;
};

} // namespace llvm
```

Construct the generated policy and return it from `Toy16Subtarget.cpp`:

```cpp
#include "Toy16Subtarget.h"
#include "GISel/Toy16LegalizerInfo.h"

using namespace llvm;

Toy16Subtarget::Toy16Subtarget(const Triple &TT, StringRef CPU, StringRef FS,
                               const TargetMachine &TM)
    : Toy16GenSubtargetInfo(TT, CPU, CPU, FS) {
  // Construct the target's other GlobalISel components here as usual.
  (void)TM;
  Legalizer = std::make_unique<Toy16LegalizerInfo>();
}

const LegalizerInfo *Toy16Subtarget::getLegalizerInfo() const {
  return Legalizer.get();
}
```

If the target already owns a `std::unique_ptr<LegalizerInfo>`, only replace its
old construction with `std::make_unique<Toy16LegalizerInfo>()`; do not add a
second legalizer.

## 5. Ensure the GlobalISel pipeline is present

An existing GlobalISel target normally already has these hooks. A new target
needs the standard pass sequence in its `TargetPassConfig`:

```cpp
bool Toy16PassConfig::addIRTranslator() {
  addPass(new IRTranslator());
  return false;
}

bool Toy16PassConfig::addLegalizeMachineIR() {
  addPass(new Legalizer());
  return false;
}

bool Toy16PassConfig::addRegBankSelect() {
  addPass(new RegBankSelect());
  return false;
}

bool Toy16PassConfig::addGlobalInstructionSelect() {
  addPass(new InstructionSelect());
  return false;
}
```

Register the GlobalISel passes once during target initialization:

```cpp
extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void LLVMInitializeToy16Target() {
  RegisterTargetMachine<Toy16TargetMachine> X(getTheToy16Target());
  initializeGlobalISel(*PassRegistry::getPassRegistry());
}
```

These pipeline hooks are prerequisites of GlobalISel as a whole; the generated
legalizer does not replace them.

## 6. Validate the integration

Build the target and first stop immediately after legalization:

```sh
cmake --build <llvm-build> --target LLVMToy16CodeGen llc
<llvm-build>/bin/llc -mtriple=toy16 -global-isel \
  -verify-machineinstrs -stop-after=legalizer input.ll -o -
```

Inspect that, for native integers `[16, 32]`:

- an `i8 G_CONSTANT` is promoted to the `i16` carrier while `i16` remains legal;
- `i1` and `i8` integer operations are promoted to `i16`;
- an `i24` integer operation is promoted to `i32`;
- the constrained `G_MUL` accepts `i16`, widens `i8` to `i16`, and rejects
  `i32` instead of treating every native integer type as legal;
- the multi-index `G_SHL` accepts `i16` or `i32` at type index 0 but only
  `i16` at type index 1; an `i8` shift amount widens to `i16`, while an `i32`
  shift amount is rejected;
- an `f16 G_FCONSTANT` becomes a bit-identical `i16 G_CONSTANT` with no
  remaining `G_BITCAST` when `f16` is not native;
- an ordinary `f16 G_FADD`, `G_FSUB`, `G_FMUL`, or `G_FDIV` is evaluated as
  `f32` and truncated back to `f16`, and an `f16 G_FCMP` compares exactly
  extended `f32` inputs and returns `i32`;
- an exact floating-point vector `G_FADD`, such as `v4f32`, passes legalization
  unchanged without a vector entry in the scalar capability table; the target
  selector must support that complete vector operation;
- the constrained `G_FDIV` accepts `f32` and widens `f16` to `f32`; adding a
  wider global native float without listing it for `G_FDIV` does not make that
  type legal for division;
- an `f16 G_FCONSTANT` feeding one of those promoted operations is folded with
  its `G_FPEXT` into an exact `f32 G_FCONSTANT`, leaving no low-precision
  constant definition on that path;
- a nonconstant `f16` listed intrinsic argument becomes an `i16` bit carrier,
  while a `G_FCONSTANT f16` argument becomes a bit-identical `G_CONSTANT i16`
  directly, without a `G_BITCAST`;
- an `i64 G_CONSTANT` intrinsic argument with candidates `[i16, i32]` becomes
  `i16` when its value fits either the signed or unsigned `i16` range and falls
  through to `i32` otherwise; a value outside both ranges and a nonconstant
  `i64` are rejected;
- a plain non-atomic `G_LOAD/G_STORE` with an unsupported `f16` value uses an
  equal-width `i16` memory carrier, while its MMO width and alignment stay
  unchanged;
- a plain non-atomic vector `G_LOAD/G_STORE`, such as `v4i16`, is directly legal
  when its value type and MMO memory type are identical; vector types do not
  need entries in the scalar index-0 candidate list and are not widened,
  bitcast, or split by this template;
- a plain non-atomic, non-volatile `G_LOAD/G_STORE i1` uses one complete slot
  of the narrowest native integer (`i16` in this example), changing both value
  and MMO types to `i16`; with `ZeroOrOneBooleanContent`, a load/store round
  trip leaves no `G_AND 1` after artifact combining;
- an `i8` load/store remains unsupported rather than being changed into a
  mismatched `value i16, memory i8` operation;
- a native register type omitted from the corresponding `G_LOAD` or `G_STORE`
  index-0 constraint is not directly legal for that memory opcode;
- `G_PTR_ADD` keeps its pointer type and promotes an `i8` offset to `i16`; any
  generated signed extension is normalized to `G_ANYEXT`;
- pointer constants, frame/global/constant-pool/block/jump-table addresses,
  indirect branches, pointer `G_PHI`, and exact pointer-valued loads/stores
  pass type legalization unchanged and remain covered by instruction
  selection;
- every accepted scalar `G_SELECT`, including a pointer-valued one, becomes a
  `G_BRCOND`/`G_BR` diamond with a `G_PHI` in its merge block;
- `nnan nsz G_FMAXIMUM f32` becomes one `G_FCMP ogt` followed by that control-
  flow expansion; strict `G_FMAXIMUM` additionally retains NaN propagation and
  `+0.0 > -0.0` repair paths, but leaves no `G_FMAXIMUM` or `G_SELECT`;
- an unconditional `G_BR` passes legality unchanged;
- an integer `icmp` result uses the same carrier as its legalized inputs:
  `(i1, i8)` and `(i1, i16)` become `(i16, i16)`, while `(i1, i32)` becomes
  `(i32, i32)`; pointer inputs remain unchanged and use an `i16` result;
- with `ZeroOrOneBooleanContent`, an `icmp -> zext i16 -> store i16` chain
  contains only `G_ICMP i16` and `G_STORE i16` after artifact combining, with
  no `G_AND` mask;
- `G_BRCOND` accepts both `i16` and `i32` conditions directly; a standalone
  `i1` or `i8` condition is promoted to `i16` with `G_ANYEXT`, using the
  complete native integer list rather than an
  opcode-specific capability subset;
- a branch using an integer `G_ICMP` result chooses the same predicted carrier
  as that comparison, so an `i32` comparison reaches `G_BRCOND i32` without an
  intervening conversion after artifact combining;
- a branch using an `f32 G_FCMP` result likewise reaches `G_BRCOND i32`; an
  `f16 G_FCMP` promoted to `f32` also returns and branches on `i32`;
- a value wider than `i32` fails closed instead of narrowing.

Then run through instruction selection:

```sh
<llvm-build>/bin/llc -mtriple=toy16 -global-isel \
  -verify-machineinstrs input.ll -o /dev/null
```

The target selector or lowering must cover every built-in opcode that can reach
it, including `G_FCMP`, `G_ICMP`, `G_BRCOND`, `G_BR`, and `G_PHI`, as well as
`G_BITCAST`, `G_ANYEXT`, integer truncation, and the generated floating-point
extension/truncation artifacts introduced by the policy. `G_ZEXT` and `G_SEXT`
do not reach the selector for supported integer extension pairs. It does not
need a selector pattern for `G_FMAXIMUM` or scalar `G_SELECT` because generated
legalization eliminates both.
Successfully constructing `Toy16LegalizerInfo` alone does not make those
operations selectable.
