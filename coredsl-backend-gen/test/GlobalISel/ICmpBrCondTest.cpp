#include "ExampleLegalizerInfo.h"

#include "llvm/CodeGen/GlobalISel/GISelChangeObserver.h"
#include "llvm/CodeGen/GlobalISel/GISelValueTracking.h"
#include "llvm/CodeGen/GlobalISel/Legalizer.h"
#include "llvm/CodeGen/GlobalISel/LegalizerHelper.h"
#include "llvm/CodeGen/GlobalISel/LostDebugLocObserver.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"
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

static void legalizeAll(TestContext &TC, MachineFunction &MF) {
  MachineIRBuilder B(MF);
  LostDebugLocObserver LocObserver("icmp-brcond-test");
  GISelValueTracking VT(MF);
  const auto Result = Legalizer::legalizeMachineFunction(
      MF, TC.LI, {&LocObserver}, LocObserver, B, &VT);
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
  outs() << "ICMP/BRCOND direct-carrier and boundary tests passed\n";
}
