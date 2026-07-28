add_llvm_component_group(Tiny32)

set(LLVM_TARGET_DEFINITIONS Tiny32.td)

tablegen(LLVM Tiny32GenGlobalISel.inc -gen-global-isel)
tablegen(LLVM Tiny32GenInstrInfo.inc -gen-instr-info)
tablegen(LLVM Tiny32GenRegisterBank.inc -gen-register-bank)
tablegen(LLVM Tiny32GenRegisterInfo.inc -gen-register-info)
tablegen(LLVM Tiny32GenSubtargetInfo.inc -gen-subtarget)

add_public_tablegen_target(Tiny32CommonTableGen)

add_llvm_target(Tiny32CodeGen
  Tiny32Target.cpp

  LINK_COMPONENTS
  CodeGen
  Core
  GlobalISel
  MC
  Tiny32Info
  Support
  Target
  TargetParser

  ADD_TO_COMPONENT
  Tiny32
  )

add_subdirectory(TargetInfo)
