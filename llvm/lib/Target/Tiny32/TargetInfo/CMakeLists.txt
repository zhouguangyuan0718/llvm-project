add_llvm_component_library(LLVMTiny32Info
  Tiny32TargetInfo.cpp

  LINK_COMPONENTS
  MC
  Support

  ADD_TO_COMPONENT
  Tiny32
  )
