#include "ExampleLegalizerInfo.h"

#include "llvm/CodeGen/GlobalISel/CSEInfo.h"
#include "llvm/CodeGen/GlobalISel/CSEMIRBuilder.h"
#include "llvm/CodeGen/GlobalISel/GISelChangeObserver.h"
#include "llvm/CodeGen/GlobalISel/GISelValueTracking.h"
#include "llvm/CodeGen/GlobalISel/Legalizer.h"
#include "llvm/CodeGen/GlobalISel/LegalizerHelper.h"
#include "llvm/CodeGen/GlobalISel/LostDebugLocObserver.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"
#include "llvm/CodeGen/GlobalISel/Utils.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

using namespace llvm;

static void fail(StringRef Message, const MachineFunction &MF) {
  errs() << "FAIL: " << Message << '\n';
  MF.print(errs());
  std::exit(1);
}

struct TestContext {
  LLVMContext Context;
  Module M;
  std::unique_ptr<TargetMachine> TM;
  MachineModuleInfo MMI;
  ExampleLegalizerInfo LI;

  explicit TestContext(std::unique_ptr<TargetMachine> TM)
      : M("icmp-brcond", Context), TM(std::move(TM)), MMI(this->TM.get()) {
    M.setTargetTriple(this->TM->getTargetTriple());
    M.setDataLayout(this->TM->createDataLayout());
  }

  MachineFunction &makeFunction(StringRef Name) {
    Function *F =
        Function::Create(FunctionType::get(Type::getVoidTy(Context), false),
                         GlobalValue::ExternalLinkage, Name, M);
    return MMI.getOrCreateMachineFunction(*F);
  }
};

static void legalizeAll(TestContext &TC, MachineFunction &MF,
                        bool UseCSE = false) {
  MachineIRBuilder B(MF);
  LostDebugLocObserver LocObserver("icmp-brcond-test");
  GISelValueTracking VT(MF);
  GISelCSEInfo CSEInfo;
  SmallVector<GISelChangeObserver *, 2> Observers{&LocObserver};
  if (UseCSE) {
    CSEInfo.setCSEConfig(std::make_unique<CSEConfigFull>());
    CSEInfo.analyze(MF);
    B.setCSEInfo(&CSEInfo);
    Observers.push_back(&CSEInfo);
  }
  CSEMIRBuilder CSEBuilder(B.getState());
  MachineIRBuilder &Builder = UseCSE ? CSEBuilder : B;
  const auto Result = Legalizer::legalizeMachineFunction(
      MF, TC.LI, Observers, LocObserver, Builder, &VT);
  if (Result.FailedOn)
    fail("whole-function legalization failed", MF);
}

enum class Bridge { Direct, ZExt, SExt, AnyExt, Trunc, Copies };

static void checkCompareBranch(TestContext &TC, unsigned InputBits, Bridge Kind,
                               bool CompareFirst, bool OtherUse,
                               bool CrossBlock, CmpInst::Predicate Predicate,
                               bool FullPassFirst = false) {
  MachineFunction &MF = TC.makeFunction("compare_branch");
  MachineBasicBlock *Entry = MF.CreateMachineBasicBlock();
  MachineBasicBlock *BranchBlock =
      CrossBlock ? MF.CreateMachineBasicBlock() : Entry;
  MachineBasicBlock *Target = MF.CreateMachineBasicBlock();
  MF.push_back(Entry);
  if (CrossBlock) {
    MF.push_back(BranchBlock);
    Entry->addSuccessor(BranchBlock);
  }
  MF.push_back(Target);
  BranchBlock->addSuccessor(Target);
  MachineIRBuilder B(MF);
  B.setMBB(*Entry);
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const LLT InputTy = LLT::integer(InputBits);
  const LLT CarrierTy = LLT::integer(InputBits < 16 ? 16 : InputBits);
  const LLT ResultTy = Kind == Bridge::Trunc ? CarrierTy : LLT::integer(1);
  Register LHS = B.buildUndef(InputTy).getReg(0);
  Register RHS = B.buildUndef(InputTy).getReg(0);
  MachineInstr *Cmp = B.buildICmp(Predicate, ResultTy, LHS, RHS).getInstr();
  const Register OriginalResult = Cmp->getOperand(0).getReg();
  MachineInstr *Other = nullptr;
  if (OtherUse) {
    Register Sink = MRI.createGenericVirtualRegister(ResultTy);
    Other = B.buildCopy(Sink, OriginalResult).getInstr();
  }
  if (CrossBlock) {
    B.buildBr(*BranchBlock);
    B.setMBB(*BranchBlock);
  }
  Register Condition = OriginalResult;
  switch (Kind) {
  case Bridge::Direct:
    break;
  case Bridge::ZExt:
    Condition = B.buildZExt(LLT::integer(16), Condition).getReg(0);
    break;
  case Bridge::SExt:
    Condition = B.buildSExt(LLT::integer(16), Condition).getReg(0);
    break;
  case Bridge::AnyExt:
    Condition = B.buildAnyExt(LLT::integer(16), Condition).getReg(0);
    break;
  case Bridge::Trunc:
    Condition = B.buildTrunc(LLT::integer(8), Condition).getReg(0);
    break;
  case Bridge::Copies:
    // More than the old prediction helper's eight-instruction look-through.
    for (unsigned I = 0; I != 16; ++I) {
      Register Copy = MRI.createGenericVirtualRegister(ResultTy);
      B.buildCopy(Copy, Condition);
      Condition = Copy;
    }
    break;
  }
  MachineInstr *Branch = B.buildBrCond(Condition, *Target).getInstr();
  if (FullPassFirst) {
    legalizeAll(TC, MF);
    if (Branch->getOperand(0).getReg() != Cmp->getOperand(0).getReg())
      fail("native branch bridge survived whole-function legalization", MF);
    return;
  }
  GISelObserverWrapper Observer;
  LegalizerHelper Helper(MF, TC.LI, Observer, B);
  LostDebugLocObserver LocObserver("icmp-brcond-step");
  if (CompareFirst) {
    for (unsigned I = 0; I != 3; ++I) {
      auto Result = Helper.legalizeInstrStep(*Cmp, LocObserver);
      if (Result == LegalizerHelper::AlreadyLegal)
        break;
      if (Result != LegalizerHelper::Legalized)
        fail("comparison could not be legalized first", MF);
    }
  }

  if (Helper.legalizeInstrStep(*Branch, LocObserver) ==
      LegalizerHelper::UnableToLegalize)
    fail("branch legalization failed", MF);
  const Register WideResult = Cmp->getOperand(0).getReg();
  if (TC.LI.getAction(*Cmp, MRI).Action != LegalizeActions::Legal ||
      MRI.getType(WideResult) != CarrierTy ||
      Branch->getOperand(0).getReg() != WideResult)
    fail("BRCOND did not directly reuse the legalized ICMP result", MF);
  if (Cmp->getOperand(1).getPredicate() != Predicate ||
      Branch->getOperand(1).getMBB() != Target ||
      (Other && (Other->getOperand(1).getReg() != OriginalResult ||
                 MRI.getType(OriginalResult) != ResultTy)))
    fail("comparison semantics, branch target or another use changed", MF);

  // Full worklist processing must retain the direct edge, independent of which
  // of the two instructions was visited first above.
  legalizeAll(TC, MF);
  if (Branch->getOperand(0).getReg() != Cmp->getOperand(0).getReg())
    fail("whole-function legalization reintroduced a branch conversion", MF);
  if (Other && MRI.getType(Other->getOperand(1).getReg()) != ResultTy)
    fail("another user's narrow type was not preserved", MF);
}

static void checkBarrier(TestContext &TC, unsigned Opcode) {
  MachineFunction &MF = TC.makeFunction("compare_barrier");
  MachineBasicBlock *Entry = MF.CreateMachineBasicBlock();
  MachineBasicBlock *Body = MF.CreateMachineBasicBlock();
  MachineBasicBlock *Target = MF.CreateMachineBasicBlock();
  MF.push_back(Entry);
  MF.push_back(Body);
  MF.push_back(Target);
  Entry->addSuccessor(Body);
  Body->addSuccessor(Target);
  MachineIRBuilder B(MF);
  B.setMBB(*Entry);
  const LLT I1 = LLT::integer(1), I32 = LLT::integer(32);
  Register A = B.buildUndef(I32).getReg(0);
  MachineInstr *Cmp = B.buildICmp(CmpInst::ICMP_EQ, I1, A, A).getInstr();
  Register OriginalResult = Cmp->getOperand(0).getReg();
  B.buildBr(*Body);
  B.setMBB(*Body);
  Register Condition;
  if (Opcode == TargetOpcode::G_PHI)
    Condition = B.buildInstr(Opcode, {I1}, {})
                    .addUse(OriginalResult)
                    .addMBB(Entry)
                    .getReg(0);
  else
    Condition = B.buildInstr(Opcode, {I1}, {OriginalResult}).getReg(0);
  MachineInstr *Branch = B.buildBrCond(Condition, *Target).getInstr();
  GISelObserverWrapper Observer;
  LegalizerHelper Helper(MF, TC.LI, Observer, B);
  LostDebugLocObserver LocObserver("compare-barrier-step");
  if (Helper.legalizeInstrStep(*Branch, LocObserver) ==
          LegalizerHelper::UnableToLegalize ||
      Cmp->getOperand(0).getReg() != OriginalResult ||
      MF.getRegInfo().getType(OriginalResult) != I1)
    fail("branch crossed a PHI or FREEZE boundary", MF);
}

static void checkFloatCompare(TestContext &TC) {
  MachineFunction &MF = TC.makeFunction("float_compare_unchanged");
  MachineBasicBlock *Entry = MF.CreateMachineBasicBlock();
  MachineBasicBlock *Target = MF.CreateMachineBasicBlock();
  MF.push_back(Entry);
  MF.push_back(Target);
  Entry->addSuccessor(Target);
  MachineIRBuilder B(MF);
  B.setMBB(*Entry);
  Register A = B.buildUndef(LLT::floatIEEE(32)).getReg(0);
  MachineInstr *Cmp =
      B.buildFCmp(CmpInst::FCMP_OEQ, LLT::integer(1), A, A).getInstr();
  MachineInstr *Branch =
      B.buildBrCond(Cmp->getOperand(0).getReg(), *Target).getInstr();
  legalizeAll(TC, MF);
  Register Result = Cmp->getOperand(0).getReg();
  if (MF.getRegInfo().getType(Result) != LLT::integer(32) ||
      Branch->getOperand(0).getReg() != Result)
    fail("the existing FCMP branch policy changed", MF);
}

static void checkNonCompare(TestContext &TC, unsigned Bits, bool Arithmetic) {
  MachineFunction &MF = TC.makeFunction("non_compare_branch");
  MachineBasicBlock *Entry = MF.CreateMachineBasicBlock();
  MachineBasicBlock *Target = MF.CreateMachineBasicBlock();
  MF.push_back(Entry);
  MF.push_back(Target);
  Entry->addSuccessor(Target);
  MachineIRBuilder B(MF);
  B.setMBB(*Entry);
  MachineRegisterInfo &MRI = MF.getRegInfo();
  LLT Ty = LLT::integer(Bits);
  Register Condition = B.buildUndef(Ty).getReg(0);
  MachineInstr *Cmp = nullptr;
  if (Arithmetic) {
    Register A = B.buildUndef(LLT::integer(32)).getReg(0);
    Cmp = B.buildICmp(CmpInst::ICMP_EQ, LLT::integer(1), A, A).getInstr();
    Register Extended = B.buildZExt(Ty, Cmp->getOperand(0).getReg()).getReg(0);
    Condition = B.buildXor(Ty, Extended, Condition).getReg(0);
  }
  MachineInstr *Branch = B.buildBrCond(Condition, *Target).getInstr();
  GISelObserverWrapper Observer;
  LegalizerHelper Helper(MF, TC.LI, Observer, B);
  LostDebugLocObserver LocObserver("non-compare-step");
  const auto Size = Entry->size();
  auto Result = Helper.legalizeInstrStep(*Branch, LocObserver);
  if (Bits > 32) {
    if (Result != LegalizerHelper::UnableToLegalize || Entry->size() != Size)
      fail("oversized non-compare condition did not fail closed", MF);
    return;
  }
  if (Result == LegalizerHelper::UnableToLegalize ||
      MRI.getType(Branch->getOperand(0).getReg()) !=
          LLT::integer(Bits < 16 ? 16 : Bits))
    fail("non-compare branch lost its original carrier policy", MF);
  if (Bits >= 16 && Branch->getOperand(0).getReg() != Condition)
    fail("already-native non-compare condition was rewritten", MF);
  if (Cmp && MRI.getType(Cmp->getOperand(0).getReg()) != LLT::integer(1))
    fail("branch bypassed an arithmetic operation", MF);
}

static void checkAndOne(TestContext &TC, unsigned Bits, bool MaskFirst,
                        bool OtherUse, bool FullPassFirst,
                        unsigned MaskAdapter = 0) {
  MachineFunction &MF = TC.makeFunction("and_one_branch");
  MachineBasicBlock *Entry = MF.CreateMachineBasicBlock();
  MachineBasicBlock *Target = MF.CreateMachineBasicBlock();
  MF.push_back(Entry);
  MF.push_back(Target);
  Entry->addSuccessor(Target);
  MachineIRBuilder B(MF);
  B.setMBB(*Entry);
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const LLT Ty = LLT::integer(Bits);
  const LLT CarrierTy = LLT::integer(Bits < 16 ? 16 : Bits);
  Register Input = B.buildUndef(Ty).getReg(0);
  Register One =
      B.buildConstant(
           MaskAdapter == TargetOpcode::G_ZEXT ? LLT::integer(1) : Ty, 1)
          .getReg(0);
  if (MaskAdapter == TargetOpcode::COPY) {
    Register Copied = MRI.createGenericVirtualRegister(Ty);
    B.buildCopy(Copied, One);
    One = Copied;
  } else if (MaskAdapter == TargetOpcode::G_ZEXT) {
    One = B.buildZExt(Ty, One).getReg(0);
  }
  MachineInstr *And =
      B.buildAnd(Ty, MaskFirst ? One : Input, MaskFirst ? Input : One)
          .getInstr();
  const Register OriginalResult = And->getOperand(0).getReg();
  MachineInstr *Other = nullptr;
  if (OtherUse) {
    Register Sink = MRI.createGenericVirtualRegister(Ty);
    Other = B.buildCopy(Sink, OriginalResult).getInstr();
  }
  Register Condition = OriginalResult;
  if (Bits > 1)
    Condition = B.buildTrunc(LLT::integer(1), Condition).getReg(0);
  // The branch already has a native carrier, just like the original ICMP bug.
  Condition = B.buildZExt(LLT::integer(16), Condition).getReg(0);
  MachineInstr *Branch = B.buildBrCond(Condition, *Target).getInstr();
  if (!FullPassFirst) {
    GISelObserverWrapper Observer;
    LegalizerHelper Helper(MF, TC.LI, Observer, B);
    LostDebugLocObserver LocObserver("and-one-step");
    if (Helper.legalizeInstrStep(*Branch, LocObserver) ==
        LegalizerHelper::UnableToLegalize)
      fail("AND-by-one branch could not be legalized", MF);
    if (TC.LI.getAction(*And, MRI).Action != LegalizeActions::Legal ||
        Branch->getOperand(0).getReg() != And->getOperand(0).getReg())
      fail("branch did not directly use AND-by-one after a single step", MF);
    const auto Size = Entry->size();
    Helper.legalizeInstrStep(*Branch, LocObserver);
    if (Entry->size() != Size)
      fail("AND-by-one rewrite is not idempotent", MF);
  }
  legalizeAll(TC, MF);
  const Register Result = And->getOperand(0).getReg();
  if (MRI.getType(Result) != CarrierTy ||
      Branch->getOperand(0).getReg() != Result)
    fail("AND-by-one retained a branch truncation/extension", MF);
  const Register MaskReg = And->getOperand(MaskFirst ? 1 : 2).getReg();
  const auto Constant = getIConstantVRegValWithLookThrough(MaskReg, MRI);
  if (!Constant || !Constant->Value.isOne() ||
      MRI.getType(MaskReg) != CarrierTy)
    fail("AND-by-one mask gained unspecified or sign-extended upper bits", MF);
  if (Other && MRI.getType(Other->getOperand(1).getReg()) != Ty)
    fail("AND-by-one changed another user's original type", MF);
}

static void checkOtherAndMasks(TestContext &TC, int MaskValue, bool Variable,
                               bool AnyExtended) {
  MachineFunction &MF = TC.makeFunction("other_and_mask");
  MachineBasicBlock *Entry = MF.CreateMachineBasicBlock();
  MachineBasicBlock *Target = MF.CreateMachineBasicBlock();
  MF.push_back(Entry);
  MF.push_back(Target);
  Entry->addSuccessor(Target);
  MachineIRBuilder B(MF);
  B.setMBB(*Entry);
  const LLT I32 = LLT::integer(32);
  Register Input = B.buildUndef(I32).getReg(0);
  Register Mask;
  if (Variable)
    Mask = B.buildUndef(I32).getReg(0);
  else if (AnyExtended) {
    Mask = B.buildConstant(LLT::integer(1), 1).getReg(0);
    Mask = B.buildAnyExt(I32, Mask).getReg(0);
  } else
    Mask = B.buildConstant(I32, MaskValue).getReg(0);
  MachineInstr *And = B.buildAnd(I32, Input, Mask).getInstr();
  Register Result = And->getOperand(0).getReg();
  Register Narrow = B.buildTrunc(LLT::integer(1), Result).getReg(0);
  Register Condition = B.buildZExt(LLT::integer(16), Narrow).getReg(0);
  MachineInstr *Branch = B.buildBrCond(Condition, *Target).getInstr();
  GISelObserverWrapper Observer;
  LegalizerHelper Helper(MF, TC.LI, Observer, B);
  LostDebugLocObserver LocObserver("other-and-mask-step");
  const auto Size = Entry->size();
  if (Helper.legalizeInstrStep(*Branch, LocObserver) ==
          LegalizerHelper::UnableToLegalize ||
      Branch->getOperand(0).getReg() != Condition || Entry->size() != Size ||
      And->getOperand(2).getReg() != Mask)
    fail("AND with a mask other than exact one was rewritten", MF);
}

static void checkURem(TestContext &TC, unsigned Bits, uint64_t DivisorValue,
                      bool Dynamic = false, bool Unknown = false,
                      bool Signed = false, bool BranchUse = false) {
  MachineFunction &MF = TC.makeFunction("urem_power_of_two");
  MachineBasicBlock *Entry = MF.CreateMachineBasicBlock();
  MF.push_back(Entry);
  MachineIRBuilder B(MF);
  B.setMBB(*Entry);
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const LLT Ty = LLT::integer(Bits);
  Register Input = B.buildUndef(Ty).getReg(0);
  Register Divisor;
  if (Unknown)
    Divisor = B.buildUndef(Ty).getReg(0);
  else if (Dynamic) {
    Register One = B.buildConstant(Ty, 1).getReg(0);
    Register Shift = B.buildUndef(LLT::integer(16)).getReg(0);
    Divisor = B.buildShl(Ty, One, Shift).getReg(0);
  } else {
    Register Constant = B.buildConstant(Ty, DivisorValue).getReg(0);
    Divisor = MRI.createGenericVirtualRegister(Ty);
    B.buildCopy(Divisor, Constant);
  }
  MachineInstr *Rem =
      B.buildInstr(Signed ? TargetOpcode::G_SREM : TargetOpcode::G_UREM, {Ty},
                   {Input, Divisor})
          .getInstr();
  Register OriginalResult = Rem->getOperand(0).getReg();
  Register Sink = MRI.createGenericVirtualRegister(Ty);
  MachineInstr *Other = B.buildCopy(Sink, OriginalResult).getInstr();
  MachineInstr *Branch = nullptr;
  if (BranchUse) {
    MachineBasicBlock *Target = MF.CreateMachineBasicBlock();
    MF.push_back(Target);
    Entry->addSuccessor(Target);
    Register Narrow = B.buildTrunc(LLT::integer(1), OriginalResult).getReg(0);
    Register Condition = B.buildZExt(LLT::integer(16), Narrow).getReg(0);
    Branch = B.buildBrCond(Condition, *Target).getInstr();
  }
  const bool Expected = !Signed && !Unknown &&
                        (Dynamic || APInt(Bits, DivisorValue).isPowerOf2());
  legalizeAll(TC, MF);
  if (Rem->getOpcode() != (Expected ? TargetOpcode::G_AND
                           : Signed ? TargetOpcode::G_SREM
                                    : TargetOpcode::G_UREM) ||
      Rem->getOperand(0).getReg() != OriginalResult ||
      Rem->getOperand(1).getReg() != Input ||
      Other->getOperand(1).getReg() != OriginalResult)
    fail("remainder rewrite changed opcode or preserved operands incorrectly",
         MF);
  if (!Expected) {
    if (Rem->getOperand(2).getReg() != Divisor)
      fail("non-power-of-two remainder divisor changed", MF);
    return;
  }
  Register MaskReg = Rem->getOperand(2).getReg();
  if (Dynamic) {
    MachineInstr *Add = MRI.getVRegDef(MaskReg);
    if (Add->getOpcode() != TargetOpcode::G_ADD ||
        Add->getOperand(1).getReg() != Divisor)
      fail("dynamic power-of-two remainder did not use divisor minus one", MF);
    auto NegOne = getIConstantVRegVal(Add->getOperand(2).getReg(), MRI);
    if (!NegOne || !NegOne->isAllOnes())
      fail("dynamic remainder mask addend is not minus one", MF);
  } else {
    auto Mask = getIConstantVRegVal(MaskReg, MRI);
    if (!Mask || *Mask != APInt(Bits, DivisorValue) - 1)
      fail("constant power-of-two remainder has the wrong mask", MF);
  }
  if (Branch && Branch->getOperand(0).getReg() != OriginalResult)
    fail("remainder by two did not connect AND one directly to BRCOND", MF);
}

// Model missing replacement operations without changing the example's policy.
struct URemDependencyInfo : LegalizerInfo {
  const ExampleLegalizerInfo &Delegate;
  URemDependencyInfo(const ExampleLegalizerInfo &Delegate, unsigned Missing)
      : Delegate(Delegate) {
    getActionDefinitionsBuilder(TargetOpcode::G_UREM).custom();
    for (unsigned Op :
         {TargetOpcode::G_AND, TargetOpcode::G_CONSTANT, TargetOpcode::G_ADD}) {
      auto &Rules = getActionDefinitionsBuilder(Op);
      if (Op == Missing)
        Rules.unsupported();
      else
        Rules.alwaysLegal();
    }
  }
  bool legalizeCustom(LegalizerHelper &Helper, MachineInstr &MI,
                      LostDebugLocObserver &Observer) const override {
    return Delegate.legalizeCustom(Helper, MI, Observer);
  }
};

static void checkURemDependencies(TestContext &TC, unsigned Missing,
                                  bool Dynamic) {
  MachineFunction &MF = TC.makeFunction("urem_missing_dependency");
  MachineBasicBlock *Entry = MF.CreateMachineBasicBlock();
  MF.push_back(Entry);
  MachineIRBuilder B(MF);
  B.setMBB(*Entry);
  const LLT Ty = LLT::integer(32);
  Register Input = B.buildUndef(Ty).getReg(0);
  Register Divisor = B.buildConstant(Ty, Dynamic ? 1 : 8).getReg(0);
  if (Dynamic)
    Divisor = B.buildShl(Ty, Divisor, Input).getReg(0);
  MachineInstr *Rem = B.buildURem(Ty, Input, Divisor).getInstr();
  URemDependencyInfo LI(TC.LI, Missing);
  GISelObserverWrapper Observer;
  LegalizerHelper Helper(MF, LI, Observer, B);
  LostDebugLocObserver LocObserver("urem-dependency-step");
  const auto Size = Entry->size();
  bool Expected = !Dynamic && Missing == TargetOpcode::G_ADD;
  if (Helper.legalizeInstrStep(*Rem, LocObserver) !=
          LegalizerHelper::Legalized ||
      Rem->getOpcode() !=
          (Expected ? TargetOpcode::G_AND : TargetOpcode::G_UREM))
    fail("remainder replacement dependency guard failed", MF);
  if (!Expected &&
      (Entry->size() != Size || Rem->getOperand(2).getReg() != Divisor))
    fail("skipped remainder rewrite left partial instructions behind", MF);
}

static void checkSelectCompareBranch(TestContext &TC, unsigned InputBits,
                                     unsigned ValueBits, bool CompareFirst,
                                     bool NativeAdapter, bool OtherUse,
                                     bool FullPassFirst,
                                     CmpInst::Predicate Predicate) {
  MachineFunction &MF = TC.makeFunction("select_compare_branch");
  MachineBasicBlock *Entry = MF.CreateMachineBasicBlock();
  MachineBasicBlock *Exit = MF.CreateMachineBasicBlock();
  MF.push_back(Entry);
  MF.push_back(Exit);
  Entry->addSuccessor(Exit);
  MachineIRBuilder B(MF);
  B.setMBB(*Entry);
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const LLT InputTy = LLT::integer(InputBits),
            ValueTy = LLT::integer(ValueBits);
  Register A = B.buildUndef(InputTy).getReg(0);
  Register C = B.buildUndef(InputTy).getReg(0);
  MachineInstr *Cmp = B.buildICmp(Predicate, LLT::integer(1), A, C).getInstr();
  Register Condition = Cmp->getOperand(0).getReg();
  MachineInstr *Other = nullptr;
  if (OtherUse) {
    Register Sink = MRI.createGenericVirtualRegister(LLT::integer(1));
    Other = B.buildCopy(Sink, Condition).getInstr();
  }
  if (NativeAdapter)
    Condition = B.buildZExt(LLT::integer(16), Condition).getReg(0);
  Register TrueValue = B.buildConstant(ValueTy, 1).getReg(0);
  Register FalseValue = B.buildConstant(ValueTy, 0).getReg(0);
  MachineInstr *Select =
      B.buildSelect(ValueTy, Condition, TrueValue, FalseValue).getInstr();
  Register Selected = Select->getOperand(0).getReg();
  B.buildBr(*Exit);
  B.setMBB(*Exit);
  MachineInstr *ExitPhi = B.buildInstr(TargetOpcode::G_PHI, {ValueTy}, {})
                              .addUse(Selected)
                              .addMBB(Entry)
                              .getInstr();
  Register Sink = MRI.createGenericVirtualRegister(ValueTy);
  B.buildCopy(Sink, ExitPhi->getOperand(0).getReg());
  if (!FullPassFirst) {
    GISelObserverWrapper Observer;
    LegalizerHelper Helper(MF, TC.LI, Observer, B);
    LostDebugLocObserver LocObserver("select-compare-step");
    if (CompareFirst) {
      for (unsigned I = 0; I != 3 && TC.LI.getAction(*Cmp, MRI).Action !=
                                         LegalizeActions::Legal;
           ++I)
        if (Helper.legalizeInstrStep(*Cmp, LocObserver) !=
            LegalizerHelper::Legalized)
          fail("SELECT's comparison could not be legalized first", MF);
    }
    // Data widening may precede expansion; condition widening must not.
    if (ValueBits < 16 && Helper.legalizeInstrStep(*Select, LocObserver) !=
                              LegalizerHelper::Legalized)
      fail("SELECT data widening failed", MF);
    if (Helper.legalizeInstrStep(*Select, LocObserver) !=
        LegalizerHelper::Legalized)
      fail("SELECT expansion failed", MF);
    MachineInstr *Branch = Entry->getFirstTerminatorForward() == Entry->end()
                               ? nullptr
                               : &*Entry->getFirstTerminatorForward();
    if (!Branch || Branch->getOpcode() != TargetOpcode::G_BRCOND ||
        Branch->getOperand(0).getReg() != Cmp->getOperand(0).getReg() ||
        TC.LI.getAction(*Cmp, MRI).Action != LegalizeActions::Legal)
      fail("SELECT did not create a direct legalized comparison branch", MF);
  }
  legalizeAll(TC, MF, FullPassFirst);
  MachineInstr &Branch = *Entry->getFirstTerminatorForward();
  if (Branch.getOpcode() != TargetOpcode::G_BRCOND ||
      Branch.getOperand(0).getReg() != Cmp->getOperand(0).getReg() ||
      MRI.getType(Branch.getOperand(0).getReg()) !=
          LLT::integer(std::max(16u, InputBits)) ||
      Cmp->getOperand(1).getPredicate() != Predicate)
    fail("SELECT-generated branch retained condition artifacts", MF);
  for (const auto &BB : MF)
    for (const auto &MI : BB)
      // Narrow comparison inputs may legitimately need masking during their
      // own widening. Native input comparisons need no mask at all here.
      if (MI.getOpcode() == TargetOpcode::G_SELECT ||
          (InputBits >= 16 && MI.getOpcode() == TargetOpcode::G_AND))
        fail("comparison SELECT left a SELECT or boolean mask behind", MF);
  if (Other && MRI.getType(Other->getOperand(1).getReg()) != LLT::integer(1))
    fail("SELECT expansion changed another comparison user's type", MF);
  if (Exit->pred_size() != 1 ||
      ExitPhi->getOperand(2).getMBB() != *Exit->pred_begin() ||
      ExitPhi->getOperand(2).getMBB() == Entry || MF.size() != 5)
    fail("SELECT expansion did not repair the successor PHI and CFG", MF);
}

static void checkSelectComparisons(TestContext &TC, unsigned InputBits) {
  SmallVector<unsigned, 5> ValueWidths{1, 8, 16, 32};
  if (InputBits == 64)
    ValueWidths.push_back(64);
  for (unsigned ValueBits : ValueWidths)
    for (bool CompareFirst : {false, true})
      for (bool NativeAdapter : {false, true})
        for (bool OtherUse : {false, true})
          for (bool FullPassFirst : {false, true})
            for (auto Pred :
                 {CmpInst::ICMP_EQ, CmpInst::ICMP_SLT, CmpInst::ICMP_ULT})
              checkSelectCompareBranch(TC, InputBits, ValueBits, CompareFirst,
                                       NativeAdapter, OtherUse, FullPassFirst,
                                       Pred);
}

static void checkTruncBranchProof(TestContext &TC, int MaskValue,
                                  unsigned Adapter, unsigned Bits = 32,
                                  unsigned CopyDepth = 0, bool Swap = false) {
  MachineFunction &MF = TC.makeFunction("trunc_branch_proof");
  MachineBasicBlock *Entry = MF.CreateMachineBasicBlock();
  MachineBasicBlock *Target = MF.CreateMachineBasicBlock();
  MF.push_back(Entry);
  MF.push_back(Target);
  Entry->addSuccessor(Target);
  MachineIRBuilder B(MF);
  B.setMBB(*Entry);
  auto &MRI = MF.getRegInfo();
  LLT Ty = LLT::integer(Bits), I1 = LLT::integer(1);
  Register A = B.buildUndef(Ty).getReg(0);
  Register C = B.buildUndef(Ty).getReg(0);
  Register Mask;
  if (Adapter == TargetOpcode::G_ANYEXT || Adapter == TargetOpcode::G_ZEXT ||
      Adapter == TargetOpcode::G_SEXT) {
    Register One = B.buildConstant(I1, 1).getReg(0);
    Mask = B.buildInstr(Adapter, {Ty}, {One}).getReg(0);
  } else if (Adapter == TargetOpcode::G_IMPLICIT_DEF) {
    Mask = B.buildUndef(Ty).getReg(0);
  } else {
    Mask = B.buildConstant(Ty, MaskValue).getReg(0);
  }
  MachineInstr *Inner =
      B.buildAnd(Ty, Swap ? Mask : C, Swap ? C : Mask).getInstr();
  Register DynamicMask = Inner->getOperand(0).getReg();
  if (Adapter == TargetOpcode::G_FREEZE)
    DynamicMask = B.buildInstr(Adapter, {Ty}, {DynamicMask}).getReg(0);
  else if (Adapter == TargetOpcode::G_OR || Adapter == TargetOpcode::G_XOR) {
    Register OtherMasked = B.buildAnd(Ty, A, Mask).getReg(0);
    DynamicMask =
        B.buildInstr(Adapter, {Ty}, {DynamicMask, OtherMasked}).getReg(0);
  }
  for (unsigned I = 0; I != CopyDepth; ++I) {
    Register Copied = MRI.createGenericVirtualRegister(Ty);
    B.buildCopy(Copied, DynamicMask);
    DynamicMask = Copied;
  }
  MachineInstr *Outer =
      B.buildAnd(Ty, Swap ? DynamicMask : A, Swap ? A : DynamicMask).getInstr();
  Register Value = Outer->getOperand(0).getReg();
  MachineInstr *Trunc = B.buildTrunc(I1, Value).getInstr();
  Register Narrow = Trunc->getOperand(0).getReg();
  Register Sink = MRI.createGenericVirtualRegister(I1);
  MachineInstr *Other = B.buildCopy(Sink, Narrow).getInstr();
  MachineInstr *Branch = B.buildBrCond(Narrow, *Target).getInstr();
  GISelObserverWrapper Observer;
  LegalizerHelper Helper(MF, TC.LI, Observer, B);
  LostDebugLocObserver LocObserver("trunc-branch-proof-step");
  const bool Expected = Bits == 32 &&
                        (!Adapter || Adapter == TargetOpcode::G_OR ||
                         Adapter == TargetOpcode::G_XOR) &&
                        CopyDepth < 64 && (MaskValue == 0 || MaskValue == 1);
  const auto Size = Entry->size();
  for (unsigned I = 0; I != 2; ++I)
    if (Helper.legalizeInstrStep(*Trunc, LocObserver) !=
            LegalizerHelper::Legalized ||
        Branch->getOperand(0).getReg() != (Expected ? Value : Narrow) ||
        Entry->size() != Size)
      fail("late truncation proof failed or was not idempotent", MF);
  if (Outer->getOperand(Swap ? 2 : 1).getReg() != A ||
      Outer->getOperand(Swap ? 1 : 2).getReg() != DynamicMask ||
      Inner->getOperand(Swap ? 1 : 2).getReg() != Mask ||
      Other->getOperand(1).getReg() != Narrow || MRI.getType(Narrow) != I1)
    fail("late truncation proof changed arithmetic or another user", MF);
}

static void checkLateSelectAnd(TestContext &TC, bool UseCSE, bool OtherUse,
                               bool SelectFirst) {
  MachineFunction &MF = TC.makeFunction("issue_22_select_and");
  MachineBasicBlock *Entry = MF.CreateMachineBasicBlock();
  MF.push_back(Entry);
  MachineIRBuilder B(MF);
  B.setMBB(*Entry);
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const LLT I1 = LLT::integer(1), I16 = LLT::integer(16),
            I32 = LLT::integer(32);
  Register Input = B.buildUndef(I16).getReg(0);
  Register Extended = B.buildAnyExt(I32, Input).getReg(0);
  Register Two = B.buildConstant(I32, 2).getReg(0);
  // The issue does not show %85's definition. Remainder by two provides a
  // concrete producer that legalizes later into exactly its posted AND-one
  // mask, rather than starting the test with an already formed AND chain.
  MachineInstr *Remainder = B.buildURem(I32, Extended, Two).getInstr();
  Register Right =
      B.buildTrunc(I1, Remainder->getOperand(0).getReg()).getReg(0);
  Register A = B.buildUndef(I32).getReg(0);
  Register C = B.buildUndef(I32).getReg(0);
  MachineInstr *Cmp = B.buildICmp(CmpInst::ICMP_SLE, I1, A, C).getInstr();
  MachineInstr *And =
      B.buildAnd(I1, Cmp->getOperand(0).getReg(), Right).getInstr();
  Register Condition = And->getOperand(0).getReg();
  MachineInstr *Other = nullptr;
  if (OtherUse) {
    Register Sink = MRI.createGenericVirtualRegister(I1);
    Other = B.buildCopy(Sink, Condition).getInstr();
  }
  const LLT F32 = LLT::floatIEEE(32);
  Register TrueValue = B.buildFConstant(F32, 1.0).getReg(0);
  Register FalseValue = B.buildFConstant(F32, 2.0).getReg(0);
  MachineInstr *Select =
      B.buildSelect(F32, Condition, TrueValue, FalseValue).getInstr();
  Register Sink = MRI.createGenericVirtualRegister(F32);
  B.buildCopy(Sink, Select->getOperand(0).getReg());
  if (SelectFirst) {
    GISelObserverWrapper Observer;
    LegalizerHelper Helper(MF, TC.LI, Observer, B);
    LostDebugLocObserver LocObserver("issue-22-select-first");
    if (Helper.legalizeInstrStep(*Select, LocObserver) !=
        LegalizerHelper::Legalized)
      fail("issue 22 SELECT expansion failed", MF);
  }
  legalizeAll(TC, MF, UseCSE);
  MachineInstr &Branch = *Entry->getFirstTerminatorForward();
  Register WideAnd = And->getOperand(0).getReg();
  if (Branch.getOpcode() != TargetOpcode::G_BRCOND ||
      Branch.getOperand(0).getReg() != WideAnd || MRI.getType(WideAnd) != I32)
    fail("issue 22 retained TRUNC i32-to-i1 on the generated branch", MF);
  if (Remainder->getOpcode() != TargetOpcode::G_AND ||
      And->getOperand(2).getReg() != Remainder->getOperand(0).getReg())
    fail("issue 22 dynamic mask was replaced or remainder was not lowered", MF);
  MachineInstr *CmpTrunc = MRI.getVRegDef(And->getOperand(1).getReg());
  if (CmpTrunc->getOpcode() != TargetOpcode::G_TRUNC ||
      CmpTrunc->getOperand(1).getReg() != Cmp->getOperand(0).getReg() ||
      MRI.getType(Cmp->getOperand(0).getReg()) != LLT::integer(64))
    fail("issue 22 changed the comparison-to-i32 AND conversion", MF);
  if (Other && (Other->getOperand(1).getReg() != Condition ||
                MRI.getType(Condition) != I1))
    fail("issue 22 changed another user's original i1 value", MF);
  if (!OtherUse)
    for (const auto &BB : MF)
      for (const auto &MI : BB)
        if (MI.getOpcode() == TargetOpcode::G_TRUNC &&
            MRI.getType(MI.getOperand(0).getReg()) == I1)
          fail("issue 22 left a dead boolean truncation behind", MF);
}

int main() {
  LLT::setUseExtended(true);
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  const Triple TT(sys::getDefaultTargetTriple());
  std::string Error;
  const Target *T = TargetRegistry::lookupTarget(TT, Error);
  if (!T) {
    errs() << Error << '\n';
    return 1;
  }
  TargetOptions Options;
  std::unique_ptr<TargetMachine> TM(
      T->createTargetMachine(TT, "generic", "", Options, std::nullopt,
                             std::nullopt, CodeGenOptLevel::Default));
  if (!TM)
    return 1;
  TestContext TC(std::move(TM));
#ifdef COREDSL_SELECT_AND_TEST
  for (int Mask : {0, 1, 2, 3, -1})
    for (bool Swap : {false, true}) {
      for (unsigned Depth : {0, 8, 128})
        checkTruncBranchProof(TC, Mask, 0, 32, Depth, Swap);
      for (unsigned Op : {TargetOpcode::G_OR, TargetOpcode::G_XOR})
        checkTruncBranchProof(TC, Mask, Op, 32, 0, Swap);
    }
  for (unsigned Adapter :
       {TargetOpcode::G_ANYEXT, TargetOpcode::G_ZEXT, TargetOpcode::G_SEXT,
        TargetOpcode::G_FREEZE, TargetOpcode::G_IMPLICIT_DEF})
    checkTruncBranchProof(TC, 1, Adapter);
  checkTruncBranchProof(TC, 1, 0, 16);
  for (bool UseCSE : {false, true})
    for (bool OtherUse : {false, true})
      for (bool SelectFirst : {false, true})
        checkLateSelectAnd(TC, UseCSE, OtherUse, SelectFirst);
  outs() << "late SELECT/AND branch truncation tests passed\n";
  return 0;
#endif
#ifdef COREDSL_SELECT_I64_TEST
  checkSelectComparisons(TC, 64);
  outs() << "i64 comparison SELECT-generated branch tests passed\n";
  return 0;
#endif
  for (unsigned Bits : {8u, 16u, 32u})
    checkSelectComparisons(TC, Bits);
  for (unsigned Bits : {16u, 32u}) {
    for (uint64_t Divisor :
         {uint64_t(0), uint64_t(1), uint64_t(2), uint64_t(3), uint64_t(8),
          uint64_t(1) << (Bits - 1), (uint64_t(1) << Bits) - 1})
      checkURem(TC, Bits, Divisor);
    checkURem(TC, Bits, 0, true);
    checkURem(TC, Bits, 0, false, true);
    checkURem(TC, Bits, 8, false, false, true);
    checkURem(TC, Bits, 2, false, false, false, true);
  }
  for (unsigned Missing :
       {TargetOpcode::G_AND, TargetOpcode::G_CONSTANT, TargetOpcode::G_ADD})
    for (bool Dynamic : {false, true})
      checkURemDependencies(TC, Missing, Dynamic);
  for (unsigned Bits : {1u, 8u, 16u, 32u})
    for (bool MaskFirst : {false, true})
      for (bool FullPassFirst : {false, true})
        checkAndOne(TC, Bits, MaskFirst, true, FullPassFirst);
  checkAndOne(TC, 32, false, false, false, TargetOpcode::COPY);
  checkAndOne(TC, 32, true, false, true, TargetOpcode::G_ZEXT);
  for (int Mask : {0, 2, 3, -1})
    checkOtherAndMasks(TC, Mask, false, false);
  checkOtherAndMasks(TC, 0, true, false);
  checkOtherAndMasks(TC, 1, false, true);
  checkCompareBranch(TC, 32, Bridge::ZExt, false, false, false,
                     CmpInst::ICMP_EQ, true);
  for (unsigned Bits : {8u, 16u, 32u})
    for (bool CompareFirst : {false, true})
      checkCompareBranch(TC, Bits, Bridge::Direct, CompareFirst, false, false,
                         CmpInst::ICMP_EQ);
  for (Bridge Kind : {Bridge::ZExt, Bridge::SExt, Bridge::AnyExt, Bridge::Trunc,
                      Bridge::Copies})
    for (bool CompareFirst : {false, true})
      checkCompareBranch(TC, 32, Kind, CompareFirst, false, false,
                         CmpInst::ICMP_NE);
  for (bool CompareFirst : {false, true}) {
    checkCompareBranch(TC, 32, Bridge::ZExt, CompareFirst, true, false,
                       CmpInst::ICMP_EQ);
    checkCompareBranch(TC, 32, Bridge::Copies, CompareFirst, true, true,
                       CmpInst::ICMP_EQ);
  }
  for (auto Predicate : {CmpInst::ICMP_SLT, CmpInst::ICMP_ULT})
    checkCompareBranch(TC, 8, Bridge::Direct, false, false, false, Predicate);
  for (unsigned Bits : {1u, 8u, 16u, 32u, 64u, 128u})
    checkNonCompare(TC, Bits, false);
  checkNonCompare(TC, 16, true);
  checkBarrier(TC, TargetOpcode::G_PHI);
  checkBarrier(TC, TargetOpcode::G_FREEZE);
  checkFloatCompare(TC);
  outs() << "ICMP/AND-one BRCOND and power-of-two UREM tests passed\n";
}
