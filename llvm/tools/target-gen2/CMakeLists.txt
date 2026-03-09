set(LLVM_LINK_COMPONENTS
  Support
)

add_llvm_tool(target-gen2
  target-gen2.cpp
  TargetGen2JSON.cpp
  TargetGen2Parser.cpp
)
