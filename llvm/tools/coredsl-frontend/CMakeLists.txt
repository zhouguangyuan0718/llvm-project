set(LLVM_LINK_COMPONENTS
  Support
  )

add_llvm_tool(coredsl-frontend
  IRPrinter.cpp
  Main.cpp
  Parser.cpp
  )
