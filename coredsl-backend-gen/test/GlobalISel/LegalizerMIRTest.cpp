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
#include "llvm/IR/Intrinsics.h"
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

static void fail(StringRef Message, const MachineFunction *MF = nullptr) {
  errs() << "FAIL: " << Message << '\n';
  if (MF)
    MF->print(errs());
  std::exit(1);
}

struct TestContext {
  LLVMContext Context;
  Module M;
  std::unique_ptr<TargetMachine> TM;
  MachineModuleInfo MMI;
  ExampleLegalizerInfo LI;

  explicit TestContext(std::unique_ptr<TargetMachine> TM)
      : M("local-carrier-test", Context), TM(std::move(TM)),
        MMI(this->TM.get()) {
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

static MachineInstr *findUniqueOpcode(MachineFunction &MF, unsigned Opcode) {
  MachineInstr *Found = nullptr;
  for (MachineBasicBlock &MBB : MF) {
    for (MachineInstr &MI : MBB) {
      if (MI.getOpcode() != Opcode)
        continue;
      if (Found)
        fail("expected exactly one opcode", &MF);
      Found = &MI;
    }
  }
  if (!Found)
    fail("expected opcode was not found", &MF);
  return Found;
}

static void keepAlive(MachineIRBuilder &B, MachineRegisterInfo &MRI,
                      Register Reg) {
  Register Sink = MRI.createGenericVirtualRegister(MRI.getType(Reg));
  B.buildCopy(Sink, Reg);
}

static void legalizeAll(TestContext &TC, MachineFunction &MF) {
  MachineIRBuilder B(MF);
  LostDebugLocObserver LocObserver("local-carrier-test");
  GISelValueTracking VT(MF);
  const Legalizer::MFResult Result = Legalizer::legalizeMachineFunction(
      MF, TC.LI, {&LocObserver}, LocObserver, B, &VT);
  if (Result.FailedOn) {
    errs() << "failed instruction: " << *Result.FailedOn;
    fail("whole-function legalization failed", &MF);
  }
}

static void checkSingleUseCoalescing(TestContext &TC) {
  MachineFunction &MF = TC.makeFunction("single_use");
  MachineBasicBlock *MBB = MF.CreateMachineBasicBlock();
  MF.push_back(MBB);
  MachineIRBuilder B(MF);
  B.setMBB(*MBB);
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const LLT S8 = LLT::integer(8);

  Register A = MRI.createGenericVirtualRegister(S8);
  Register BReg = MRI.createGenericVirtualRegister(S8);
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {A}, {});
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {BReg}, {});
  Register AddDst = B.buildAdd(S8, A, BReg).getReg(0);
  Register C = MRI.createGenericVirtualRegister(S8);
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {C}, {});
  Register XorDst = B.buildXor(S8, AddDst, C).getReg(0);
  keepAlive(B, MRI, XorDst);

  legalizeAll(TC, MF);
  MachineInstr *Add = findUniqueOpcode(MF, TargetOpcode::G_ADD);
  MachineInstr *Xor = findUniqueOpcode(MF, TargetOpcode::G_XOR);
  if (MRI.getType(Add->getOperand(0).getReg()) != LLT::integer(16) ||
      MRI.getType(Xor->getOperand(0).getReg()) != LLT::integer(16))
    fail("generic chain consulted the configured XOR s32 constraint", &MF);
  if (Xor->getOperand(1).getReg() != Add->getOperand(0).getReg() &&
      Xor->getOperand(2).getReg() != Add->getOperand(0).getReg())
    fail("an artifact remained between the generic producer and consumer", &MF);
}

static void checkCopyLookThrough(TestContext &TC) {
  MachineFunction &MF = TC.makeFunction("copy_lookthrough");
  MachineBasicBlock *MBB = MF.CreateMachineBasicBlock();
  MF.push_back(MBB);
  MachineIRBuilder B(MF);
  B.setMBB(*MBB);
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const LLT S8 = LLT::integer(8);

  Register A = MRI.createGenericVirtualRegister(S8);
  Register BReg = MRI.createGenericVirtualRegister(S8);
  Register C = MRI.createGenericVirtualRegister(S8);
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {A}, {});
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {BReg}, {});
  Register AddDst = B.buildAdd(S8, A, BReg).getReg(0);
  Register Copied = AddDst;
  for (unsigned I = 0; I != 8; ++I) {
    Register Next = MRI.createGenericVirtualRegister(S8);
    B.buildCopy(Next, Copied);
    Copied = Next;
  }
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {C}, {});
  Register XorDst = B.buildXor(S8, Copied, C).getReg(0);
  keepAlive(B, MRI, XorDst);

  legalizeAll(TC, MF);
  MachineInstr *Add = findUniqueOpcode(MF, TargetOpcode::G_ADD);
  if (MRI.getType(Add->getOperand(0).getReg()) != LLT::integer(16))
    fail("same-type COPY blocked local carrier propagation", &MF);
}

static void checkMultipleUseFallback(TestContext &TC) {
  MachineFunction &MF = TC.makeFunction("multiple_use");
  MachineBasicBlock *MBB = MF.CreateMachineBasicBlock();
  MF.push_back(MBB);
  MachineIRBuilder B(MF);
  B.setMBB(*MBB);
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const LLT S8 = LLT::integer(8);

  Register A = MRI.createGenericVirtualRegister(S8);
  Register BReg = MRI.createGenericVirtualRegister(S8);
  Register C = MRI.createGenericVirtualRegister(S8);
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {A}, {});
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {BReg}, {});
  Register AddDst = B.buildAdd(S8, A, BReg).getReg(0);
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {C}, {});
  Register XorDst = B.buildXor(S8, AddDst, C).getReg(0);
  keepAlive(B, MRI, AddDst);
  keepAlive(B, MRI, XorDst);

  legalizeAll(TC, MF);
  MachineInstr *Add = findUniqueOpcode(MF, TargetOpcode::G_ADD);
  if (MRI.getType(Add->getOperand(0).getReg()) != LLT::integer(16))
    fail("multiple-use producer did not retain the default s16 carrier", &MF);
}

static void checkConstrainedProducerFallback(TestContext &TC) {
  MachineFunction &MF = TC.makeFunction("constrained_producer");
  MachineBasicBlock *MBB = MF.CreateMachineBasicBlock();
  MF.push_back(MBB);
  MachineIRBuilder B(MF);
  B.setMBB(*MBB);
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const LLT S8 = LLT::integer(8);

  Register A = MRI.createGenericVirtualRegister(S8);
  Register BReg = MRI.createGenericVirtualRegister(S8);
  Register C = MRI.createGenericVirtualRegister(S8);
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {A}, {});
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {BReg}, {});
  Register MulDst = B.buildMul(S8, A, BReg).getReg(0);
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {C}, {});
  Register XorDst = B.buildXor(S8, MulDst, C).getReg(0);
  keepAlive(B, MRI, XorDst);

  legalizeAll(TC, MF);
  MachineInstr *Mul = findUniqueOpcode(MF, TargetOpcode::G_MUL);
  if (MRI.getType(Mul->getOperand(0).getReg()) != LLT::integer(16))
    fail("opcode constraint did not reject the consumer's s32 carrier", &MF);
}

static void checkCrossBlockFallback(TestContext &TC) {
  MachineFunction &MF = TC.makeFunction("cross_block");
  MachineBasicBlock *ProducerMBB = MF.CreateMachineBasicBlock();
  MachineBasicBlock *ConsumerMBB = MF.CreateMachineBasicBlock();
  MF.push_back(ProducerMBB);
  MF.push_back(ConsumerMBB);
  ProducerMBB->addSuccessor(ConsumerMBB);
  MachineIRBuilder B(MF);
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const LLT S8 = LLT::integer(8);

  B.setMBB(*ProducerMBB);
  Register A = MRI.createGenericVirtualRegister(S8);
  Register BReg = MRI.createGenericVirtualRegister(S8);
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {A}, {});
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {BReg}, {});
  Register AddDst = B.buildAdd(S8, A, BReg).getReg(0);
  B.buildBr(*ConsumerMBB);

  B.setMBB(*ConsumerMBB);
  Register C = MRI.createGenericVirtualRegister(S8);
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {C}, {});
  Register XorDst = B.buildXor(S8, AddDst, C).getReg(0);
  keepAlive(B, MRI, XorDst);

  legalizeAll(TC, MF);
  MachineInstr *Add = findUniqueOpcode(MF, TargetOpcode::G_ADD);
  if (MRI.getType(Add->getOperand(0).getReg()) != LLT::integer(16))
    fail("cross-block use did not retain the default s16 carrier", &MF);
}

static void checkRawDefinedExtensionBoundary(TestContext &TC) {
  MachineFunction &MF = TC.makeFunction("raw_defined_extension");
  MachineBasicBlock *MBB = MF.CreateMachineBasicBlock();
  MF.push_back(MBB);
  MachineIRBuilder B(MF);
  B.setMBB(*MBB);
  MachineRegisterInfo &MRI = MF.getRegInfo();
  GISelObserverWrapper Observer;
  LegalizerHelper Helper(MF, TC.LI, Observer, B);
  LostDebugLocObserver LocObserver("local-carrier-test");
  const LLT S8 = LLT::integer(8);
  const LLT S32 = LLT::integer(32);

  Register A = MRI.createGenericVirtualRegister(S8);
  Register BReg = MRI.createGenericVirtualRegister(S8);
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {A}, {});
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {BReg}, {});
  MachineInstr *Add = B.buildAdd(S8, A, BReg).getInstr();
  Register Extended = MRI.createGenericVirtualRegister(S32);
  B.buildZExt(Extended, Add->getOperand(0).getReg());
  keepAlive(B, MRI, Extended);

  if (Helper.legalizeInstrStep(*Add, LocObserver) != LegalizerHelper::Legalized)
    fail("direct producer legalization failed", &MF);
  MachineInstr *WideAdd = findUniqueOpcode(MF, TargetOpcode::G_ADD);
  if (MRI.getType(WideAdd->getOperand(0).getReg()) != LLT::integer(16))
    fail("raw G_ZEXT was incorrectly treated as a carrier preference", &MF);
}

static void checkBrCondConfiguredPromotion(TestContext &TC, bool Strict) {
  MachineFunction &MF = TC.makeFunction("brcond_configured_promotion");
  MachineBasicBlock *Entry = MF.CreateMachineBasicBlock();
  MachineBasicBlock *Target = MF.CreateMachineBasicBlock();
  MF.push_back(Entry);
  MF.push_back(Target);
  Entry->addSuccessor(Target);

  MachineIRBuilder B(MF);
  B.setMBB(*Entry);
  MachineRegisterInfo &MRI = MF.getRegInfo();
  Register Condition = MRI.createGenericVirtualRegister(LLT::integer(8));
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {Condition}, {});
  B.buildBrCond(Condition, *Target);

  legalizeAll(TC, MF);
  MachineInstr *BrCond = findUniqueOpcode(MF, TargetOpcode::G_BRCOND);
  if (MRI.getType(BrCond->getOperand(0).getReg()) !=
      LLT::integer(Strict ? 32 : 16))
    fail("G_BRCOND i8 did not use the selected policy's carrier", &MF);
}

static void checkBrCondCompareCarrier(TestContext &TC) {
  MachineFunction &MF = TC.makeFunction("brcond_compare_carrier");
  MachineBasicBlock *Entry = MF.CreateMachineBasicBlock();
  MachineBasicBlock *Target = MF.CreateMachineBasicBlock();
  MF.push_back(Entry);
  MF.push_back(Target);
  Entry->addSuccessor(Target);

  MachineIRBuilder B(MF);
  B.setMBB(*Entry);
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const LLT I32 = LLT::integer(32);
  Register LHS = MRI.createGenericVirtualRegister(I32);
  Register RHS = MRI.createGenericVirtualRegister(I32);
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {LHS}, {});
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {RHS}, {});
  Register Condition =
      B.buildICmp(CmpInst::ICMP_EQ, LLT::integer(1), LHS, RHS).getReg(0);
  B.buildBrCond(Condition, *Target);

  legalizeAll(TC, MF);
  MachineInstr *ICmp = findUniqueOpcode(MF, TargetOpcode::G_ICMP);
  MachineInstr *BrCond = findUniqueOpcode(MF, TargetOpcode::G_BRCOND);
  const Register ICmpResult = ICmp->getOperand(0).getReg();
  if (MRI.getType(ICmpResult) != I32 ||
      MRI.getType(BrCond->getOperand(0).getReg()) != I32 ||
      BrCond->getOperand(0).getReg() != ICmpResult)
    fail("G_BRCOND did not coalesce with its supported compare carrier", &MF);
}

static void checkSelectEmitsSupportedBrCond(TestContext &TC, bool Strict) {
  MachineFunction &MF = TC.makeFunction("select_emits_supported_brcond");
  MachineBasicBlock *Entry = MF.CreateMachineBasicBlock();
  MF.push_back(Entry);

  MachineIRBuilder B(MF);
  B.setMBB(*Entry);
  MachineRegisterInfo &MRI = MF.getRegInfo();
  Register Condition = MRI.createGenericVirtualRegister(LLT::integer(1));
  Register TrueValue = MRI.createGenericVirtualRegister(LLT::integer(32));
  Register FalseValue = MRI.createGenericVirtualRegister(LLT::integer(32));
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {Condition}, {});
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {TrueValue}, {});
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {FalseValue}, {});
  Register Result =
      B.buildSelect(LLT::integer(32), Condition, TrueValue, FalseValue)
          .getReg(0);
  keepAlive(B, MRI, Result);

  legalizeAll(TC, MF);
  for (MachineBasicBlock &MBB : MF)
    for (MachineInstr &MI : MBB)
      if (MI.getOpcode() == TargetOpcode::G_SELECT)
        fail("G_SELECT remained after control-flow expansion", &MF);
  MachineInstr *BrCond = findUniqueOpcode(MF, TargetOpcode::G_BRCOND);
  const LLT BrCondTy = MRI.getType(BrCond->getOperand(0).getReg());
  if (BrCondTy != LLT::integer(Strict ? 32 : 16))
    fail("G_SELECT emitted a G_BRCOND outside the selected policy", &MF);
}

static void checkICmpUsesWiderBrCondCarrier(TestContext &TC, bool Strict) {
  MachineFunction &MF = TC.makeFunction("icmp_uses_wider_brcond_carrier");
  MachineBasicBlock *Entry = MF.CreateMachineBasicBlock();
  MachineBasicBlock *Target = MF.CreateMachineBasicBlock();
  MF.push_back(Entry);
  MF.push_back(Target);
  Entry->addSuccessor(Target);

  MachineIRBuilder B(MF);
  B.setMBB(*Entry);
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const LLT I8 = LLT::integer(8);
  const LLT CarrierTy = LLT::integer(Strict ? 32 : 16);
  Register LHS = MRI.createGenericVirtualRegister(I8);
  Register RHS = MRI.createGenericVirtualRegister(I8);
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {LHS}, {});
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {RHS}, {});
  Register Condition =
      B.buildICmp(CmpInst::ICMP_EQ, LLT::integer(1), LHS, RHS).getReg(0);
  B.buildBrCond(Condition, *Target);

  legalizeAll(TC, MF);
  MachineInstr *ICmp = findUniqueOpcode(MF, TargetOpcode::G_ICMP);
  MachineInstr *BrCond = findUniqueOpcode(MF, TargetOpcode::G_BRCOND);
  const Register ICmpResult = ICmp->getOperand(0).getReg();
  if (MRI.getType(ICmpResult) != CarrierTy ||
      MRI.getType(ICmp->getOperand(2).getReg()) != CarrierTy ||
      MRI.getType(BrCond->getOperand(0).getReg()) != CarrierTy ||
      BrCond->getOperand(0).getReg() != ICmpResult)
    fail("G_ICMP did not adopt the wider G_BRCOND carrier", &MF);
}

static void checkFloatConstantAndComparison(TestContext &TC, bool Strict) {
  MachineFunction &MF = TC.makeFunction("float_constant_and_compare");
  MachineBasicBlock *MBB = MF.CreateMachineBasicBlock();
  MF.push_back(MBB);
  MachineIRBuilder B(MF);
  B.setMBB(*MBB);
  const LLT F32 = LLT::floatIEEE(32), F64 = LLT::floatIEEE(64);
  Register Constant = B.buildFConstant(F32, 1.5).getReg(0);
  Register Extended = B.buildFPExt(F64, Constant).getReg(0);
  keepAlive(B, MF.getRegInfo(), Extended);
  legalizeAll(TC, MF);
  MachineInstr *FC = findUniqueOpcode(MF, TargetOpcode::G_FCONSTANT);
  if (MF.getRegInfo().getType(FC->getOperand(0).getReg()) !=
          (Strict ? F64 : F32) ||
      FC->getOperand(1).getFPImm()->getValueAPF().convertToDouble() != 1.5)
    fail("explicit FCONSTANT constraint did not fold to exact f64", &MF);
  for (MachineInstr &MI : *MBB)
    if (Strict && MI.getOpcode() == TargetOpcode::G_FPEXT)
      fail("constant extension survived folding", &MF);

  MachineFunction &CompareMF = TC.makeFunction("float_compare_intersection");
  MachineBasicBlock *CompareMBB = CompareMF.CreateMachineBasicBlock();
  CompareMF.push_back(CompareMBB);
  MachineIRBuilder CB(CompareMF);
  CB.setMBB(*CompareMBB);
  Register A = CB.buildUndef(F32).getReg(0);
  Register C = CB.buildUndef(F32).getReg(0);
  Register Result =
      CB.buildFCmp(CmpInst::FCMP_OEQ, LLT::integer(1), A, C).getReg(0);
  keepAlive(CB, CompareMF.getRegInfo(), Result);
  legalizeAll(TC, CompareMF);
  MachineInstr *Cmp = findUniqueOpcode(CompareMF, TargetOpcode::G_FCMP);
  if (CompareMF.getRegInfo().getType(Cmp->getOperand(0).getReg()) !=
          LLT::integer(Strict ? 64 : 32) ||
      CompareMF.getRegInfo().getType(Cmp->getOperand(2).getReg()) !=
          (Strict ? F64 : F32))
    fail("FCMP did not use a common result/input width", &CompareMF);
}

static void checkFMaximumRemainsOrdinaryOperation(TestContext &TC) {
  MachineFunction &MF = TC.makeFunction("ordinary_fmaximum");
  MachineBasicBlock *MBB = MF.CreateMachineBasicBlock();
  MF.push_back(MBB);
  MachineIRBuilder B(MF);
  B.setMBB(*MBB);
  const LLT F32 = LLT::floatIEEE(32);
  Register A = B.buildUndef(F32).getReg(0);
  Register C = B.buildUndef(F32).getReg(0);
  Register Dst =
      B.buildInstr(TargetOpcode::G_FMAXIMUM, {F32}, {A, C}).getReg(0);
  keepAlive(B, MF.getRegInfo(), Dst);
  legalizeAll(TC, MF);
  findUniqueOpcode(MF, TargetOpcode::G_FMAXIMUM);
  if (MF.size() != 1)
    fail("FMAXIMUM unexpectedly expanded control flow", &MF);
  for (MachineInstr &MI : *MBB)
    if (MI.getOpcode() == TargetOpcode::G_FCMP ||
        MI.getOpcode() == TargetOpcode::G_SELECT)
      fail("FMAXIMUM unexpectedly emitted comparison/select", &MF);
}

static void checkURemDependencies(TestContext &TC, bool Strict, bool Missing) {
  MachineFunction &MF = TC.makeFunction("urem_dependencies");
  MachineBasicBlock *MBB = MF.CreateMachineBasicBlock();
  MF.push_back(MBB);
  MachineIRBuilder B(MF);
  B.setMBB(*MBB);
  MachineRegisterInfo &MRI = MF.getRegInfo();
  GISelObserverWrapper Observer;
  LegalizerHelper Helper(MF, TC.LI, Observer, B);
  LostDebugLocObserver LocObserver("urem-dependencies");
  const LLT I32 = LLT::integer(32);
  Register Src = B.buildUndef(I32).getReg(0);
  auto Step = [&](Register Divisor, bool ShouldRewrite) {
    MachineInstr *Rem = B.buildURem(I32, Src, Divisor).getInstr();
    if (Helper.legalizeInstrStep(*Rem, LocObserver) !=
            LegalizerHelper::Legalized ||
        Rem->getOpcode() !=
            (ShouldRewrite ? TargetOpcode::G_AND : TargetOpcode::G_UREM))
      fail("UREM dependency guard selected the wrong rewrite", &MF);
    return Rem;
  };
  const bool CanFoldConstant = !Strict || !Missing;
  MachineInstr *ConstantRem =
      Step(B.buildConstant(I32, 16).getReg(0), CanFoldConstant);
  if (CanFoldConstant) {
    MachineInstr *Mask = MRI.getVRegDef(ConstantRem->getOperand(2).getReg());
    if (Mask->getOpcode() != TargetOpcode::G_CONSTANT ||
        Mask->getOperand(1).getCImm()->getValue() != 15)
      fail("constant UREM mask must equal divisor minus one", &MF);
  }
  Step(B.buildConstant(I32, 0).getReg(0), false);
  Step(B.buildConstant(I32, 12).getReg(0), false);
  Step(B.buildUndef(I32).getReg(0), false);
  Register One = B.buildConstant(I32, 1).getReg(0);
  Register Amount = B.buildUndef(I32).getReg(0);
  Register Dynamic = B.buildShl(I32, One, Amount).getReg(0);
  // ADD is intentionally absent in both fixtures. Strict mode must not emit
  // a dynamic mask that requires its native fallback.
  MachineInstr *DynamicRem = Step(Dynamic, !Strict);
  if (!Strict) {
    MachineInstr *Mask = MRI.getVRegDef(DynamicRem->getOperand(2).getReg());
    if (Mask->getOpcode() != TargetOpcode::G_ADD)
      fail("dynamic UREM mask must subtract one", &MF);
  }
}

static void checkI1MemoryExpansion(TestContext &TC) {
  MachineFunction &MF = TC.makeFunction("i1_byte_memory");
  MachineBasicBlock *MBB = MF.CreateMachineBasicBlock();
  MF.push_back(MBB);
  MachineIRBuilder B(MF);
  B.setMBB(*MBB);
  const LLT I1 = LLT::integer(1);
  Register Ptr = B.buildUndef(LLT::pointer(0, 64)).getReg(0);
  MachineMemOperand *LoadMMO = MF.getMachineMemOperand(
      MachinePointerInfo(), MachineMemOperand::MOLoad, I1, Align(1));
  MachineMemOperand *StoreMMO = MF.getMachineMemOperand(
      MachinePointerInfo(), MachineMemOperand::MOStore, I1, Align(1));
  Register Value = B.buildLoad(I1, Ptr, *LoadMMO).getReg(0);
  B.buildStore(Value, Ptr, *StoreMMO);
  legalizeAll(TC, MF);
  unsigned Loads = 0, Stores = 0, Merges = 0;
  for (MachineInstr &MI : *MBB) {
    if (MI.getOpcode() == TargetOpcode::G_OR)
      ++Merges;
    if (MI.getOpcode() != TargetOpcode::G_LOAD &&
        MI.getOpcode() != TargetOpcode::G_STORE)
      continue;
    (MI.getOpcode() == TargetOpcode::G_LOAD ? Loads : Stores)++;
    const MachineMemOperand &MMO = **MI.memoperands_begin();
    if (MMO.getMemoryType() != LLT::integer(16) || MMO.getAlign() != Align(1))
      fail("i1 expansion changed the selected access width or alignment", &MF);
  }
  if (Loads != 2 || Stores != 1 || Merges != 1)
    fail("wide i1 store did not retain its read/modify/write sequence", &MF);
}

static void checkRejectedSelectDoesNotChangeCFG(TestContext &TC) {
  MachineFunction &MF = TC.makeFunction("select_missing_dependencies");
  MachineBasicBlock *MBB = MF.CreateMachineBasicBlock();
  MF.push_back(MBB);
  MachineIRBuilder B(MF);
  B.setMBB(*MBB);
  Register Condition = B.buildUndef(LLT::integer(16)).getReg(0);
  Register A = B.buildUndef(LLT::integer(32)).getReg(0);
  Register C = B.buildUndef(LLT::integer(32)).getReg(0);
  MachineInstr *Select =
      B.buildSelect(LLT::integer(32), Condition, A, C).getInstr();
  GISelObserverWrapper Observer;
  LegalizerHelper Helper(MF, TC.LI, Observer, B);
  LostDebugLocObserver LocObserver("select-dependencies");
  const auto Size = MBB->size();
  if (Helper.legalizeInstrStep(*Select, LocObserver) !=
          LegalizerHelper::UnableToLegalize ||
      MF.size() != 1 || MBB->size() != Size ||
      Select->getOpcode() != TargetOpcode::G_SELECT)
    fail("SELECT mutated the CFG without legal branch/PHI dependencies", &MF);
}

static void checkICmpMultipleUseBoundary(TestContext &TC) {
  MachineFunction &MF = TC.makeFunction("icmp_multiple_use_boundary");
  MachineBasicBlock *Entry = MF.CreateMachineBasicBlock();
  MachineBasicBlock *Target = MF.CreateMachineBasicBlock();
  MF.push_back(Entry);
  MF.push_back(Target);
  Entry->addSuccessor(Target);

  MachineIRBuilder B(MF);
  B.setMBB(*Entry);
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const LLT I8 = LLT::integer(8);
  Register LHS = MRI.createGenericVirtualRegister(I8);
  Register RHS = MRI.createGenericVirtualRegister(I8);
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {LHS}, {});
  B.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {RHS}, {});
  Register Condition =
      B.buildICmp(CmpInst::ICMP_EQ, LLT::integer(1), LHS, RHS).getReg(0);
  keepAlive(B, MRI, Condition);
  B.buildBrCond(Condition, *Target);

  legalizeAll(TC, MF);
  MachineInstr *ICmp = findUniqueOpcode(MF, TargetOpcode::G_ICMP);
  MachineInstr *BrCond = findUniqueOpcode(MF, TargetOpcode::G_BRCOND);
  if (MRI.getType(ICmp->getOperand(0).getReg()) != LLT::integer(16) ||
      MRI.getType(BrCond->getOperand(0).getReg()) != LLT::integer(32) ||
      BrCond->getOperand(0).getReg() == ICmp->getOperand(0).getReg())
    fail("multiple-use G_ICMP crossed the local carrier boundary", &MF);
}

static void checkIntrinsicPolicies(TestContext &TC, bool Strict, bool Missing) {
  MachineFunction &MF = TC.makeFunction("intrinsic_policies");
  MachineBasicBlock *MBB = MF.CreateMachineBasicBlock();
  MF.push_back(MBB);
  MachineIRBuilder B(MF);
  B.setMBB(*MBB);
  MachineRegisterInfo &MRI = MF.getRegInfo();
  GISelObserverWrapper Observer;
  LegalizerHelper Helper(MF, TC.LI, Observer, B);
  LostDebugLocObserver LocObserver("intrinsic-policies");
  const LLT I8 = LLT::integer(8), I16 = LLT::integer(16);
  const LLT I32 = LLT::integer(32), I64 = LLT::integer(64);
  const LLT F32 = LLT::floatIEEE(32);
  const bool Configured = Strict && !Missing;

  // Built-in IDs are opaque fixture dispatch keys here. We test the target's
  // low-bit payload convention, not these LLVM intrinsics' IR signatures or
  // their instruction selection. Exercise LLVM's actual intrinsic entry point.
  auto Step = [&](MachineInstr &MI, bool Success) {
    if (Helper.legalizeInstrStep(MI, LocObserver) !=
        (Success ? LegalizerHelper::Legalized
                 : LegalizerHelper::UnableToLegalize))
      fail("intrinsic selected the wrong legalization policy", &MF);
  };
  for (unsigned Opcode :
       {TargetOpcode::G_INTRINSIC, TargetOpcode::G_INTRINSIC_W_SIDE_EFFECTS,
        TargetOpcode::G_INTRINSIC_CONVERGENT,
        TargetOpcode::G_INTRINSIC_CONVERGENT_W_SIDE_EFFECTS}) {
    for (Intrinsic::ID ID : {Intrinsic::smax, Intrinsic::umax}) {
      B.setInsertPt(*MBB, MBB->end());
      Register Result = MRI.createGenericVirtualRegister(I8);
      Register A = B.buildUndef(I8).getReg(0);
      Register C = B.buildUndef(I8).getReg(0);
      Register Ptr = B.buildUndef(LLT::pointer(0, 64)).getReg(0);
      Register Vec = B.buildUndef(LLT::fixed_vector(4, I8)).getReg(0);
      MachineInstr *MI = B.buildInstr(Opcode)
                             .addDef(Result)
                             .addIntrinsicID(ID)
                             .addUse(A)
                             .addUse(C)
                             .addUse(Ptr)
                             .addUse(Vec)
                             .addImm(7)
                             .getInstr();
      Step(*MI, true);
      const bool HasRule = Configured && ID == Intrinsic::smax;
      const LLT Expected0 = !Strict ? I16 : HasRule ? I32 : I8;
      const LLT Expected1 = !Strict || HasRule ? I16 : I8;
      if (MI->getOpcode() != Opcode ||
          MI->getOperand(1).getIntrinsicID() != ID ||
          MRI.getType(MI->getOperand(2).getReg()) != Expected0 ||
          MRI.getType(MI->getOperand(3).getReg()) != Expected1 ||
          MI->getOperand(0).getReg() != Result || MRI.getType(Result) != I8 ||
          MI->getOperand(4).getReg() != Ptr ||
          MI->getOperand(5).getReg() != Vec || MI->getOperand(6).getImm() != 7)
        fail("intrinsic inputs or preserved operands are incorrect", &MF);
      const auto Size = MBB->size();
      Step(*MI, true);
      if (MBB->size() != Size)
        fail("intrinsic legalization is not idempotent", &MF);
    }
  }

  // Generic float inputs always use integer bit representations, even when a
  // generated entry explicitly requires f32. Test constants and nonconstants.
  for (bool Constant : {false, true}) {
    B.setInsertPt(*MBB, MBB->end());
    Register Source = Constant ? B.buildFConstant(F32, 1.5).getReg(0)
                               : B.buildUndef(F32).getReg(0);
    Register Result = MRI.createGenericVirtualRegister(F32);
    MachineInstr *MI = B.buildInstr(TargetOpcode::G_INTRINSIC)
                           .addDef(Result)
                           .addIntrinsicID(Intrinsic::sqrt)
                           .addUse(Source)
                           .getInstr();
    Step(*MI, true);
    Register Rewritten = MI->getOperand(2).getReg();
    if (MRI.getType(Rewritten) != (Strict ? F32 : I32) ||
        (Strict && Rewritten != Source) || MRI.getType(Result) != F32)
      fail("intrinsic float policy or result preservation is incorrect", &MF);
    if (!Strict) {
      MachineInstr *Def = MRI.getVRegDef(Rewritten);
      if (Def->getOpcode() !=
          (Constant ? TargetOpcode::G_CONSTANT : TargetOpcode::G_BITCAST))
        fail("intrinsic float input was not bit-represented", &MF);
      if (Constant && Def->getOperand(1).getCImm()->getValue() != 0x3fc00000)
        fail("intrinsic float constant changed its bit pattern", &MF);
    }
  }

  // No partial argument edits when a later nonconstant cannot be narrowed.
  B.setInsertPt(*MBB, MBB->end());
  Register A = B.buildUndef(I8).getReg(0);
  Register TooWide = B.buildUndef(LLT::integer(128)).getReg(0);
  MachineInstr *Rejected = B.buildInstr(TargetOpcode::G_INTRINSIC)
                               .addIntrinsicID(Intrinsic::smax)
                               .addUse(A)
                               .addUse(TooWide)
                               .getInstr();
  const auto Size = MBB->size();
  Step(*Rejected, Strict && Missing);
  if (MBB->size() != Size || Rejected->getOperand(1).getReg() != A ||
      Rejected->getOperand(2).getReg() != TooWide)
    fail("intrinsic failure left a partial rewrite", &MF);

  // Generated constant narrowing retains its signed-or-unsigned fit rule.
  for (int64_t Value : {-1, 65535, 65536}) {
    B.setInsertPt(*MBB, MBB->end());
    Register First = B.buildUndef(I32).getReg(0);
    Register Source = B.buildConstant(I64, Value).getReg(0);
    MachineInstr *MI = B.buildInstr(TargetOpcode::G_INTRINSIC)
                           .addIntrinsicID(Intrinsic::smax)
                           .addUse(First)
                           .addUse(Source)
                           .getInstr();
    const bool Narrow = Configured && Value != 65536;
    Step(*MI, !Configured || Narrow);
    Register Rewritten = MI->getOperand(2).getReg();
    if (MRI.getType(Rewritten) != (Narrow ? I16 : I64) ||
        (!Narrow && Rewritten != Source))
      fail("intrinsic constant narrowing violated the selected policy", &MF);
    if (Narrow &&
        MRI.getVRegDef(Rewritten)->getOperand(1).getCImm()->getValue() != 65535)
      fail("intrinsic narrowing changed the constant payload", &MF);
  }

  // The generated fixture lacks G_CONSTANT i64. Generic mode must ignore that
  // exclusion; generated mode must reject materialization before changing MI.
  B.setInsertPt(*MBB, MBB->end());
  Register FC = B.buildFConstant(LLT::floatIEEE(64), 1.5).getReg(0);
  MachineInstr *Bits = B.buildInstr(TargetOpcode::G_INTRINSIC)
                           .addIntrinsicID(Intrinsic::ctlz)
                           .addUse(FC)
                           .getInstr();
  const auto BeforeBits = MBB->size();
  Step(*Bits, !Configured);
  if (Strict) {
    if (Bits->getOperand(1).getReg() != FC || MBB->size() != BeforeBits)
      fail("intrinsic materialized an unsupported integer constant", &MF);
  } else if (MRI.getType(Bits->getOperand(1).getReg()) != I64) {
    fail("generic intrinsic consulted generated constant constraints", &MF);
  }

  if (!Strict || Configured) {
    // Also exercise the whole-function worklist, including the newly emitted
    // extension artifacts and the producers legalized around the intrinsic.
    MachineFunction &WholeMF = TC.makeFunction("intrinsic_whole_function");
    MachineBasicBlock *WholeMBB = WholeMF.CreateMachineBasicBlock();
    WholeMF.push_back(WholeMBB);
    MachineIRBuilder WB(WholeMF);
    WB.setMBB(*WholeMBB);
    Register Left = WB.buildUndef(I8).getReg(0);
    Register Right = WB.buildUndef(I8).getReg(0);
    Register Result = WholeMF.getRegInfo().createGenericVirtualRegister(I32);
    WB.buildInstr(TargetOpcode::G_INTRINSIC)
        .addDef(Result)
        .addIntrinsicID(Intrinsic::smax)
        .addUse(Left)
        .addUse(Right);
    keepAlive(WB, WholeMF.getRegInfo(), Result);
    legalizeAll(TC, WholeMF);
    MachineInstr *MI = findUniqueOpcode(WholeMF, TargetOpcode::G_INTRINSIC);
    if (WholeMF.getRegInfo().getType(MI->getOperand(2).getReg()) !=
            (Configured ? I32 : I16) ||
        WholeMF.getRegInfo().getType(MI->getOperand(3).getReg()) != I16)
      fail("whole-function intrinsic used the wrong carrier policy", &WholeMF);
  }
}

void runMIRTests(bool Strict, bool Missing) {
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  const Triple TT(sys::getDefaultTargetTriple());
  std::string Error;
  const Target *T = TargetRegistry::lookupTarget(TT, Error);
  if (!T)
    fail(Error);
  TargetOptions Options;
  std::unique_ptr<TargetMachine> TM(
      T->createTargetMachine(TT, "generic", "", Options, std::nullopt,
                             std::nullopt, CodeGenOptLevel::Default));
  if (!TM)
    fail("could not construct the native test TargetMachine");
  TestContext TC(std::move(TM));
  checkIntrinsicPolicies(TC, Strict, Missing);
  checkURemDependencies(TC, Strict, Missing);
  if (Missing && Strict) {
    checkRejectedSelectDoesNotChangeCFG(TC);
    return;
  }
  if (!Strict) {
    checkSingleUseCoalescing(TC);
    checkCopyLookThrough(TC);
    checkMultipleUseFallback(TC);
    checkCrossBlockFallback(TC);
    checkRawDefinedExtensionBoundary(TC);
  }
  if (Strict) {
    checkConstrainedProducerFallback(TC);
    checkICmpMultipleUseBoundary(TC);
  }
  checkBrCondConfiguredPromotion(TC, Strict);
  checkBrCondCompareCarrier(TC);
  checkICmpUsesWiderBrCondCarrier(TC, Strict);
  checkSelectEmitsSupportedBrCond(TC, Strict);
  checkFloatConstantAndComparison(TC, Strict);
  checkFMaximumRemainsOrdinaryOperation(TC);
  checkI1MemoryExpansion(TC);
}
