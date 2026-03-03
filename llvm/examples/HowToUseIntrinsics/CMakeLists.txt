set(LLVM_LINK_COMPONENTS
  Core
  Support
  )

add_llvm_example(HowToUseIntrinsics
  MemRefIntrinsicExample.cpp
  )
