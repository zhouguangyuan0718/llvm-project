//===- GMIRPointerInfo.cpp - GMIR pointer expression analysis ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/GlobalISel/GMIRPointerInfo.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallDenseSet.h"
#include "llvm/CodeGen/GlobalISel/GenericMachineInstrs.h"
#include "llvm/CodeGen/GlobalISel/LoadStoreOpt.h"
#include "llvm/CodeGen/GlobalISel/Utils.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cassert>
#include <utility>

using namespace llvm;
using namespace llvm::GISelAddressing;

namespace {

using VisitingSet = SmallDenseSet<Register, 16>;

static int64_t checkedAdd(int64_t LHS, int64_t RHS) {
  int64_t Result;
  bool Overflow = AddOverflow(LHS, RHS, Result);
  assert(!Overflow && "GMIR linear offset addition overflow");
  (void)Overflow;
  return Result;
}

static int64_t checkedSub(int64_t LHS, int64_t RHS) {
  int64_t Result;
  bool Overflow = SubOverflow(LHS, RHS, Result);
  assert(!Overflow && "GMIR linear offset subtraction overflow");
  (void)Overflow;
  return Result;
}

static int64_t checkedMul(int64_t LHS, int64_t RHS) {
  int64_t Result;
  bool Overflow = MulOverflow(LHS, RHS, Result);
  assert(!Overflow && "GMIR linear offset multiplication overflow");
  (void)Overflow;
  return Result;
}

static Register getRegIgnoringCopies(Register Reg, MachineRegisterInfo &MRI) {
  SmallDenseSet<Register, 8> Seen;

  while (Reg.isVirtual() && MRI.hasOneDef(Reg) && Seen.insert(Reg).second) {
    MachineInstr *Def = MRI.getVRegDef(Reg);
    if (!Def || !Def->isCopy() || Def->getNumOperands() < 2)
      break;

    const MachineOperand &Dst = Def->getOperand(0);
    const MachineOperand &Src = Def->getOperand(1);
    if (!Dst.isReg() || Dst.getSubReg() || !Src.isReg() || Src.getSubReg())
      break;

    Reg = Src.getReg();
  }

  return Reg;
}

static std::optional<int64_t>
getInt64Constant(Register Reg, const MachineRegisterInfo &MRI) {
  auto Constant = getIConstantVRegValWithLookThrough(Reg, MRI);
  if (!Constant)
    return std::nullopt;

  return Constant->Value.getSExtValue();
}

template <typename InfoT, typename AnalyzeFn>
static std::optional<InfoT>
getCommonPHIInputInfo(const GPhi &Phi, MachineRegisterInfo &MRI,
                      AnalyzeFn Analyze) {
  Register PhiReg = getRegIgnoringCopies(Phi.getReg(0), MRI);
  std::optional<InfoT> CommonInfo;

  for (unsigned I = 0, E = Phi.getNumIncomingValues(); I != E; ++I) {
    Register IncomingReg = Phi.getIncomingValue(I);

    // Ignore loop-carried identity edges, including COPY chains ending at the
    // PHI result. A PHI containing only self references is not simplified.
    if (getRegIgnoringCopies(IncomingReg, MRI) == PhiReg)
      continue;

    InfoT Info = Analyze(IncomingReg);
    if (!CommonInfo) {
      CommonInfo = std::move(Info);
      continue;
    }

    if (*CommonInfo != Info)
      return std::nullopt;
  }

  return CommonInfo;
}

template <typename InfoT, typename AnalyzeFn>
static std::optional<InfoT>
getCommonSelectInputInfo(const GSelect &Select, AnalyzeFn Analyze) {
  InfoT TrueInfo = Analyze(Select.getTrueReg());
  InfoT FalseInfo = Analyze(Select.getFalseReg());
  if (TrueInfo != FalseInfo)
    return std::nullopt;

  return TrueInfo;
}

static void printLinearOffset(raw_ostream &OS,
                              const GMIRLinearOffset &Offset) {
  bool HasOutput = false;

  auto PrintSign = [&](bool IsNegative) {
    if (!HasOutput) {
      if (IsNegative)
        OS << '-';
      return;
    }

    OS << (IsNegative ? " - " : " + ");
  };

  for (const GMIRLinearOffset::Term &Term : Offset.getTerms()) {
    assert(Term.Scale != 0 && "Unexpected zero-scale offset term");

    bool IsNegative = Term.Scale < 0;
    int64_t AbsScale = IsNegative ? -Term.Scale : Term.Scale;

    PrintSign(IsNegative);
    OS << printReg(Term.Reg);
    if (AbsScale != 1)
      OS << " * " << AbsScale;

    HasOutput = true;
  }

  int64_t Constant = Offset.getConstant();
  if (Constant != 0 || !HasOutput) {
    bool IsNegative = Constant < 0;
    int64_t AbsConstant = IsNegative ? -Constant : Constant;

    PrintSign(IsNegative);
    OS << AbsConstant;
  }
}

static GMIRLinearOffset
getOffsetInfoImpl(Register Reg, MachineRegisterInfo &MRI, VisitingSet &Visiting,
                  unsigned Depth, unsigned MaxDepth) {
  if (!Reg.isValid())
    return {};

  Reg = getRegIgnoringCopies(Reg, MRI);

  if (auto Constant = getInt64Constant(Reg, MRI))
    return GMIRLinearOffset::getConstant(*Constant);

  GMIRLinearOffset OpaqueReg = GMIRLinearOffset::getReg(Reg);
  if (Depth >= MaxDepth || !Reg.isVirtual() || !MRI.hasOneDef(Reg))
    return OpaqueReg;

  if (!Visiting.insert(Reg).second)
    return OpaqueReg;

  auto RemoveFromVisiting = make_scope_exit([&]() { Visiting.erase(Reg); });

  MachineInstr *Def = MRI.getVRegDef(Reg);
  if (!Def)
    return OpaqueReg;

  switch (Def->getOpcode()) {
  case TargetOpcode::G_ADD:
  case TargetOpcode::G_SUB: {
    if (Def->getNumOperands() < 3 || !Def->getOperand(1).isReg() ||
        !Def->getOperand(2).isReg())
      return OpaqueReg;

    GMIRLinearOffset LHS =
        getOffsetInfoImpl(Def->getOperand(1).getReg(), MRI, Visiting, Depth + 1,
                          MaxDepth);
    GMIRLinearOffset RHS =
        getOffsetInfoImpl(Def->getOperand(2).getReg(), MRI, Visiting, Depth + 1,
                          MaxDepth);

    if (Def->getOpcode() == TargetOpcode::G_ADD)
      LHS.add(RHS);
    else
      LHS.subtract(RHS);
    return LHS;
  }

  case TargetOpcode::G_MUL: {
    if (Def->getNumOperands() < 3 || !Def->getOperand(1).isReg() ||
        !Def->getOperand(2).isReg())
      return OpaqueReg;

    Register LHSReg = Def->getOperand(1).getReg();
    Register RHSReg = Def->getOperand(2).getReg();

    if (auto LHSConstant = getInt64Constant(LHSReg, MRI)) {
      GMIRLinearOffset RHS =
          getOffsetInfoImpl(RHSReg, MRI, Visiting, Depth + 1, MaxDepth);
      RHS.multiply(*LHSConstant);
      return RHS;
    }

    if (auto RHSConstant = getInt64Constant(RHSReg, MRI)) {
      GMIRLinearOffset LHS =
          getOffsetInfoImpl(LHSReg, MRI, Visiting, Depth + 1, MaxDepth);
      LHS.multiply(*RHSConstant);
      return LHS;
    }

    return OpaqueReg;
  }

  case TargetOpcode::G_PHI: {
    const GPhi &Phi = cast<GPhi>(*Def);
    auto CommonInfo = getCommonPHIInputInfo<GMIRLinearOffset>(
        Phi, MRI, [&](Register IncomingReg) {
          return getOffsetInfoImpl(IncomingReg, MRI, Visiting, Depth + 1,
                                   MaxDepth);
        });
    return CommonInfo.value_or(OpaqueReg);
  }

  case TargetOpcode::G_SELECT: {
    const GSelect &Select = cast<GSelect>(*Def);
    auto CommonInfo = getCommonSelectInputInfo<GMIRLinearOffset>(
        Select, [&](Register IncomingReg) {
          return getOffsetInfoImpl(IncomingReg, MRI, Visiting, Depth + 1,
                                   MaxDepth);
        });
    return CommonInfo.value_or(OpaqueReg);
  }

  default:
    return OpaqueReg;
  }
}

static GMIRPointerInfo
getPointerInfoImpl(Register Ptr, MachineRegisterInfo &MRI,
                   VisitingSet &PointerVisiting, VisitingSet &OffsetVisiting,
                   unsigned Depth, unsigned MaxDepth) {
  if (!Ptr.isValid())
    return {};

  Ptr = getRegIgnoringCopies(Ptr, MRI);
  GMIRPointerInfo OpaquePtr(Ptr);

  if (Depth >= MaxDepth || !Ptr.isVirtual() || !MRI.hasOneDef(Ptr))
    return OpaquePtr;

  if (!PointerVisiting.insert(Ptr).second)
    return OpaquePtr;

  auto RemoveFromVisiting =
      make_scope_exit([&]() { PointerVisiting.erase(Ptr); });

  MachineInstr *Def = MRI.getVRegDef(Ptr);
  if (!Def)
    return OpaquePtr;

  switch (Def->getOpcode()) {
  case TargetOpcode::G_PHI: {
    const GPhi &Phi = cast<GPhi>(*Def);
    auto CommonInfo = getCommonPHIInputInfo<GMIRPointerInfo>(
        Phi, MRI, [&](Register IncomingReg) {
          return getPointerInfoImpl(IncomingReg, MRI, PointerVisiting,
                                    OffsetVisiting, Depth + 1, MaxDepth);
        });
    return CommonInfo.value_or(OpaquePtr);
  }

  case TargetOpcode::G_SELECT: {
    const GSelect &Select = cast<GSelect>(*Def);
    auto CommonInfo = getCommonSelectInputInfo<GMIRPointerInfo>(
        Select, [&](Register IncomingReg) {
          return getPointerInfoImpl(IncomingReg, MRI, PointerVisiting,
                                    OffsetVisiting, Depth + 1, MaxDepth);
        });
    return CommonInfo.value_or(OpaquePtr);
  }

  default:
    break;
  }

  // Reuse the existing GlobalISel BaseIndexOffset parser as a one-step
  // G_PTR_ADD decoder.
  BaseIndexOffset Step = GISelAddressing::getPointerInfo(Ptr, MRI);
  if (Step.getBase() == Ptr)
    return OpaquePtr;

  GMIRPointerInfo BaseInfo =
      getPointerInfoImpl(Step.getBase(), MRI, PointerVisiting, OffsetVisiting,
                         Depth + 1, MaxDepth);

  GMIRLinearOffset ExtraOffset;
  if (Step.hasValidOffset()) {
    ExtraOffset = GMIRLinearOffset::getConstant(Step.getOffset());
  } else if (Step.getIndex().isValid()) {
    ExtraOffset = getOffsetInfoImpl(Step.getIndex(), MRI, OffsetVisiting,
                                    Depth + 1, MaxDepth);
  } else {
    return OpaquePtr;
  }

  return BaseInfo.withAddedOffset(ExtraOffset);
}

} // namespace

GMIRLinearOffset GMIRLinearOffset::getConstant(int64_t Value) {
  GMIRLinearOffset Result;
  Result.Constant = Value;
  return Result;
}

GMIRLinearOffset GMIRLinearOffset::getReg(Register Reg, int64_t Scale) {
  assert(Reg.isValid() && "Expected a valid register");

  GMIRLinearOffset Result;
  if (Scale != 0)
    Result.Terms.push_back({Reg, Scale});
  return Result;
}

void GMIRLinearOffset::addConstant(int64_t Value) {
  Constant = checkedAdd(Constant, Value);
}

void GMIRLinearOffset::addTerm(Register Reg, int64_t Scale) {
  assert(Reg.isValid() && "Expected a valid register");

  if (Scale == 0)
    return;

  auto It = std::lower_bound(Terms.begin(), Terms.end(), Reg,
                             [](const Term &LHS, Register RHS) {
                               return LHS.Reg.id() < RHS.id();
                             });

  if (It == Terms.end() || It->Reg != Reg) {
    Terms.insert(It, Term{Reg, Scale});
    return;
  }

  int64_t NewScale = checkedAdd(It->Scale, Scale);
  if (NewScale == 0)
    Terms.erase(It);
  else
    It->Scale = NewScale;
}

void GMIRLinearOffset::add(const GMIRLinearOffset &Other) {
  addConstant(Other.Constant);
  for (const Term &T : Other.Terms)
    addTerm(T.Reg, T.Scale);
}

void GMIRLinearOffset::subtract(const GMIRLinearOffset &Other) {
  Constant = checkedSub(Constant, Other.Constant);
  for (const Term &T : Other.Terms)
    addTerm(T.Reg, checkedMul(T.Scale, -1));
}

void GMIRLinearOffset::multiply(int64_t Scale) {
  if (Scale == 0) {
    Terms.clear();
    Constant = 0;
    return;
  }

  Constant = checkedMul(Constant, Scale);
  for (Term &T : Terms)
    T.Scale = checkedMul(T.Scale, Scale);
}

std::optional<int64_t>
GMIRLinearOffset::getConstantDifference(const GMIRLinearOffset &Other) const {
  if (!hasSameVariablePart(Other))
    return std::nullopt;

  return checkedSub(Other.Constant, Constant);
}

void GMIRPointerInfo::print(raw_ostream &OS) const {
  OS << "GMIRPointerInfo{";
  if (!isValid()) {
    OS << "invalid}";
    return;
  }

  OS << "base: " << printReg(BaseReg) << ", offset: ";
  printLinearOffset(OS, Offset);
  OS << '}';
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void GMIRPointerInfo::dump() const {
  print(dbgs());
  dbgs() << '\n';
}
#endif

GMIRPointerInfo
GMIRPointerInfo::withAddedOffset(const GMIRLinearOffset &ExtraOffset) const {
  assert(isValid() && "Expected a valid pointer description");

  GMIRPointerInfo Result = *this;
  Result.Offset.add(ExtraOffset);
  return Result;
}

GMIRPointerInfo GMIRPointerInfo::withAddedConstant(int64_t ExtraOffset) const {
  return withAddedOffset(GMIRLinearOffset::getConstant(ExtraOffset));
}

GMIRPointerInfo GMIRPointerInfo::withAddedReg(Register Reg, int64_t Scale) const {
  return withAddedOffset(GMIRLinearOffset::getReg(Reg, Scale));
}

std::optional<int64_t>
GMIRPointerInfo::getConstantDifference(const GMIRPointerInfo &Other) const {
  if (!isValid() || !Other.isValid() || BaseReg != Other.BaseReg)
    return std::nullopt;

  return Offset.getConstantDifference(Other.Offset);
}

GMIRPointerAnalyzer::GMIRPointerAnalyzer(MachineRegisterInfo &MRI,
                                         unsigned MaxDepth)
    : MRI(MRI), MaxDepth(MaxDepth) {
  assert(MRI.isSSA() && "GMIRPointerAnalyzer requires SSA-form MachineIR");
}

GMIRPointerInfo GMIRPointerAnalyzer::getPointerInfo(Register Ptr) const {
  VisitingSet PointerVisiting;
  VisitingSet OffsetVisiting;
  return getPointerInfoImpl(Ptr, MRI, PointerVisiting, OffsetVisiting,
                            /*Depth=*/0, MaxDepth);
}

GMIRLinearOffset GMIRPointerAnalyzer::getOffsetInfo(Register OffsetReg) const {
  VisitingSet Visiting;
  return getOffsetInfoImpl(OffsetReg, MRI, Visiting, /*Depth=*/0, MaxDepth);
}
