#include "TargetInfo/Tiny32TargetInfo.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/CodeGen/CodeGenTargetMachineImpl.h"
#include "llvm/CodeGen/GlobalISel/CallLowering.h"
#include "llvm/CodeGen/GlobalISel/IRTranslator.h"
#include "llvm/CodeGen/GlobalISel/InstructionSelect.h"
#include "llvm/CodeGen/GlobalISel/InstructionSelector.h"
#include "llvm/CodeGen/GlobalISel/Legalizer.h"
#include "llvm/CodeGen/GlobalISel/LegalizerInfo.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"
#include "llvm/CodeGen/GlobalISel/RegBankSelect.h"
#include "llvm/CodeGen/RegisterBankInfo.h"
#include "llvm/CodeGen/TargetFrameLowering.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetLowering.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/MCAsmInfoELF.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/ErrorHandling.h"

#include <memory>
#include <optional>

#define GET_REGINFO_ENUM
#include "Tiny32GenRegisterInfo.inc"
#define GET_INSTRINFO_ENUM
#include "Tiny32GenInstrInfo.inc"
#define GET_REGINFO_HEADER
#include "Tiny32GenRegisterInfo.inc"
#define GET_INSTRINFO_HEADER
#include "Tiny32GenInstrInfo.inc"
#define GET_SUBTARGETINFO_HEADER
#include "Tiny32GenSubtargetInfo.inc"
#define GET_REGBANK_DECLARATIONS
#include "Tiny32GenRegisterBank.inc"

namespace llvm {

class Tiny32Subtarget;

class Tiny32GenRegisterBankInfo : public RegisterBankInfo {
protected:
#define GET_TARGET_REGBANK_CLASS
#include "Tiny32GenRegisterBank.inc"
};

class Tiny32RegisterBankInfo final : public Tiny32GenRegisterBankInfo {
public:
  Tiny32RegisterBankInfo();
  const InstructionMapping &
  getInstrMapping(const MachineInstr &MI) const override;
};

class Tiny32LegalizerInfo final : public LegalizerInfo {
public:
  Tiny32LegalizerInfo() {
    using namespace TargetOpcode;
    getActionDefinitionsBuilder({G_ADD, G_SUB}).legalFor({LLT::scalar(32)});
  }
};

class Tiny32CallLowering final : public CallLowering {
public:
  explicit Tiny32CallLowering(const TargetLowering *TLI) : CallLowering(TLI) {}

  bool lowerReturn(MachineIRBuilder &MIRBuilder, const Value *Val,
                   ArrayRef<Register> VRegs,
                   FunctionLoweringInfo &) const override {
    if (!Val && !VRegs.empty())
      return false;
    if (Val) {
      if (VRegs.size() != 1 ||
          MIRBuilder.getMRI()->getType(VRegs[0]) != LLT::scalar(32))
        return false;
      MIRBuilder.buildCopy(Register(Tiny32::r0), VRegs[0]);
    }
    MachineInstrBuilder Return = MIRBuilder.buildInstr(Tiny32::COREDSL_RET);
    if (Val)
      Return.addUse(Register(Tiny32::r0), RegState::Implicit);
    return true;
  }

  bool lowerFormalArguments(MachineIRBuilder &MIRBuilder, const Function &F,
                            ArrayRef<ArrayRef<Register>> VRegs,
                            FunctionLoweringInfo &) const override {
    static constexpr MCPhysReg ArgRegs[] = {Tiny32::r0, Tiny32::r1, Tiny32::r2,
                                            Tiny32::r3, Tiny32::r4, Tiny32::r5,
                                            Tiny32::r6, Tiny32::r7};
    if (F.isVarArg() || VRegs.size() > 8)
      return false;
    MachineRegisterInfo &MRI = *MIRBuilder.getMRI();
    for (unsigned I = 0; I != VRegs.size(); ++I) {
      if (VRegs[I].size() != 1 || MRI.getType(VRegs[I][0]) != LLT::scalar(32))
        return false;
      MRI.addLiveIn(ArgRegs[I]);
      MIRBuilder.getMBB().addLiveIn(ArgRegs[I]);
      MIRBuilder.buildCopy(VRegs[I][0], Register(ArgRegs[I]));
    }
    return true;
  }
};

class Tiny32FrameLowering final : public TargetFrameLowering {
public:
  Tiny32FrameLowering()
      : TargetFrameLowering(StackGrowsDown, Align(1), 0, Align(1)) {}
  void emitPrologue(MachineFunction &, MachineBasicBlock &) const override {}
  void emitEpilogue(MachineFunction &, MachineBasicBlock &) const override {}

protected:
  bool hasFPImpl(const MachineFunction &) const override { return false; }
};

class Tiny32RegisterInfo final : public Tiny32GenRegisterInfo {
public:
  Tiny32RegisterInfo() : Tiny32GenRegisterInfo(Tiny32::r0) {}
  const MCPhysReg *getCalleeSavedRegs(const MachineFunction *) const override {
    return nullptr;
  }
  const uint32_t *getCallPreservedMask(const MachineFunction &,
                                       CallingConv::ID) const override {
    return nullptr;
  }
  BitVector getReservedRegs(const MachineFunction &) const override {
    return BitVector(getNumRegs());
  }
  bool eliminateFrameIndex(MachineBasicBlock::iterator, int, unsigned,
                           RegScavenger *) const override {
    report_fatal_error(
        "frame indices are outside the GlobalISel-only CoreDSL target");
  }
  Register getFrameRegister(const MachineFunction &) const override {
    return Tiny32::r0;
  }
};

class Tiny32InstrInfo final : public Tiny32GenInstrInfo {
  Tiny32RegisterInfo RI;

public:
  explicit Tiny32InstrInfo(const TargetSubtargetInfo &STI)
      : Tiny32GenInstrInfo(STI, RI) {}
  const Tiny32RegisterInfo &getRegisterInfo() const { return RI; }
};

class Tiny32InstructionSelector final : public InstructionSelector {
  const Tiny32InstrInfo &TII;
  const Tiny32RegisterInfo &TRI;
  const Tiny32RegisterBankInfo &RBI;

public:
  Tiny32InstructionSelector(const Tiny32InstrInfo &TII,
                            const Tiny32RegisterBankInfo &RBI)
      : TII(TII), TRI(TII.getRegisterInfo()), RBI(RBI) {}
  bool select(MachineInstr &MI) override {
    if (!isPreISelGenericOpcode(MI.getOpcode()))
      return true;
    switch (MI.getOpcode()) {
    case TargetOpcode::G_ADD:
      MI.setDesc(TII.get(Tiny32::ADD));
      break;
    case TargetOpcode::G_SUB:
      MI.setDesc(TII.get(Tiny32::SUB));
      break;
    default:
      return false;
    }
    constrainSelectedInstRegOperands(MI, TII, TRI, RBI);
    return true;
  }
  void setupGeneratedPerFunctionState(MachineFunction &) override {}
};

class Tiny32Subtarget final : public Tiny32GenSubtargetInfo {
  Tiny32InstrInfo InstrInfo;
  Tiny32FrameLowering FrameLowering;
  std::unique_ptr<TargetLowering> Lowering;
  std::unique_ptr<Tiny32LegalizerInfo> Legalizer;
  std::unique_ptr<Tiny32RegisterBankInfo> RegBankInfo;
  std::unique_ptr<Tiny32CallLowering> CallLoweringInfo;
  std::unique_ptr<InstructionSelector> Selector;

public:
  Tiny32Subtarget(const Triple &TT, StringRef CPU, StringRef FS,
                  const TargetMachine &TM)
      : Tiny32GenSubtargetInfo(TT, CPU, CPU, FS), InstrInfo(*this) {
    Lowering = std::make_unique<TargetLowering>(TM, *this);
    Legalizer = std::make_unique<Tiny32LegalizerInfo>();
    RegBankInfo = std::make_unique<Tiny32RegisterBankInfo>();
    CallLoweringInfo = std::make_unique<Tiny32CallLowering>(Lowering.get());
    Selector =
        std::make_unique<Tiny32InstructionSelector>(InstrInfo, *RegBankInfo);
  }
  const Tiny32InstrInfo *getInstrInfo() const override { return &InstrInfo; }
  const Tiny32RegisterInfo *getRegisterInfo() const override {
    return &InstrInfo.getRegisterInfo();
  }
  const Tiny32FrameLowering *getFrameLowering() const override {
    return &FrameLowering;
  }
  const TargetLowering *getTargetLowering() const override {
    return Lowering.get();
  }
  const LegalizerInfo *getLegalizerInfo() const override {
    return Legalizer.get();
  }
  const RegisterBankInfo *getRegBankInfo() const override {
    return RegBankInfo.get();
  }
  const CallLowering *getCallLowering() const override {
    return CallLoweringInfo.get();
  }
  InstructionSelector *getInstructionSelector() const override {
    return Selector.get();
  }
  void ParseSubtargetFeatures(StringRef CPU, StringRef TuneCPU, StringRef FS);
};

} // namespace llvm

#define DEBUG_TYPE "tiny32-subtarget"
#define GET_SUBTARGETINFO_TARGET_DESC
#define GET_SUBTARGETINFO_CTOR
#include "Tiny32GenSubtargetInfo.inc"
#undef DEBUG_TYPE

#define GET_TARGET_REGBANK_IMPL
#include "Tiny32GenRegisterBank.inc"

namespace llvm {

Tiny32RegisterBankInfo::Tiny32RegisterBankInfo()
    : Tiny32GenRegisterBankInfo() {}

const RegisterBankInfo::InstructionMapping &
Tiny32RegisterBankInfo::getInstrMapping(const MachineInstr &MI) const {
  if (!isPreISelGenericOpcode(MI.getOpcode())) {
    const InstructionMapping &Mapping = getInstrMappingImpl(MI);
    if (Mapping.isValid())
      return Mapping;
  }

  switch (MI.getOpcode()) {
  case TargetOpcode::G_ADD:
  case TargetOpcode::G_SUB:
    break;
  default:
    return getInvalidInstructionMapping();
  }

  const ValueMapping &GPRMapping =
      getValueMapping(0, 32, getRegBank(Tiny32::GPRBankID));
  const ValueMapping *Operands =
      getOperandsMapping({&GPRMapping, &GPRMapping, &GPRMapping});
  return getInstructionMapping(DefaultMappingID, 1, Operands,
                               MI.getNumOperands());
}

class Tiny32TargetMachine final : public CodeGenTargetMachineImpl {
  std::unique_ptr<TargetLoweringObjectFile> ObjectFile;
  Tiny32Subtarget Subtarget;

public:
  Tiny32TargetMachine(const Target &T, const Triple &TT, StringRef CPU,
                      StringRef FS, const TargetOptions &Options,
                      std::optional<Reloc::Model> RM,
                      std::optional<CodeModel::Model> CM, CodeGenOptLevel OL,
                      bool)
      : CodeGenTargetMachineImpl(T, TT.computeDataLayout(), TT, CPU, FS,
                                 Options, RM.value_or(Reloc::Static),
                                 CM.value_or(CodeModel::Small), OL),
        ObjectFile(std::make_unique<TargetLoweringObjectFileELF>()),
        Subtarget(TT, CPU, FS, *this) {
    initAsmInfo();
    setGlobalISel(true);
    setFastISel(false);
    setO0WantsFastISel(false);
  }
  const Tiny32Subtarget *getSubtargetImpl(const Function &) const override {
    return &Subtarget;
  }
  TargetLoweringObjectFile *getObjFileLowering() const override {
    return ObjectFile.get();
  }
  TargetPassConfig *createPassConfig(PassManagerBase &PM) override;
};

class Tiny32PassConfig final : public TargetPassConfig {
public:
  Tiny32PassConfig(Tiny32TargetMachine &TM, PassManagerBase &PM)
      : TargetPassConfig(TM, PM) {}

  bool addIRTranslator() override {
    addPass(new IRTranslator());
    return false;
  }
  bool addLegalizeMachineIR() override {
    addPass(new Legalizer());
    return false;
  }
  bool addRegBankSelect() override {
    addPass(new RegBankSelect());
    return false;
  }
  bool addGlobalInstructionSelect() override {
    addPass(new InstructionSelect());
    return false;
  }
};

TargetPassConfig *Tiny32TargetMachine::createPassConfig(PassManagerBase &PM) {
  return new Tiny32PassConfig(*this, PM);
}

} // namespace llvm

#define GET_REGINFO_MC_DESC
#include "Tiny32GenRegisterInfo.inc"
#define GET_INSTRINFO_MC_DESC
#define GET_INSTRINFO_CTOR_DTOR
#include "Tiny32GenInstrInfo.inc"
#define GET_SUBTARGETINFO_MC_DESC
#include "Tiny32GenSubtargetInfo.inc"
#define GET_REGINFO_TARGET_DESC
#include "Tiny32GenRegisterInfo.inc"

using namespace llvm;

namespace {

class Tiny32MCAsmInfo final : public MCAsmInfoELF {
public:
  Tiny32MCAsmInfo(const Triple &, const MCTargetOptions &Options)
      : MCAsmInfoELF(Options) {
    CodePointerSize = 4;
  }
};

MCInstrInfo *createTiny32MCInstrInfo() {
  auto *Info = new MCInstrInfo();
  InitTiny32MCInstrInfo(Info);
  return Info;
}

MCRegisterInfo *createTiny32MCRegisterInfo(const Triple &) {
  auto *Info = new MCRegisterInfo();
  InitTiny32MCRegisterInfo(Info, Tiny32::r0);
  return Info;
}

MCSubtargetInfo *createTiny32MCSubtargetInfo(const Triple &TT, StringRef CPU,
                                             StringRef FS) {
  return createTiny32MCSubtargetInfoImpl(TT, CPU, CPU, FS);
}

} // namespace

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void LLVMInitializeTiny32Target() {
  RegisterTargetMachine<Tiny32TargetMachine> X(getTheTiny32Target());
  initializeGlobalISel(*PassRegistry::getPassRegistry());
}
extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeTiny32TargetMC() {
  RegisterMCAsmInfo<Tiny32MCAsmInfo> A(getTheTiny32Target());
  TargetRegistry::RegisterMCInstrInfo(getTheTiny32Target(),
                                      createTiny32MCInstrInfo);
  TargetRegistry::RegisterMCRegInfo(getTheTiny32Target(),
                                    createTiny32MCRegisterInfo);
  TargetRegistry::RegisterMCSubtargetInfo(getTheTiny32Target(),
                                          createTiny32MCSubtargetInfo);
}
