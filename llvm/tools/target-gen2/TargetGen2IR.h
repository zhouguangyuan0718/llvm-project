//===- TargetGen2IR.h - CoreDSL LLVM IR emission ----------------*- C++ -*-===//

#ifndef LLVM_TOOLS_TARGET_GEN2_IR_H
#define LLVM_TOOLS_TARGET_GEN2_IR_H

#include "TargetGen2AST.h"
#include <string>

namespace llvm {
namespace targetgen2 {

std::string toLLVMIR(const Description &D);

} // namespace targetgen2
} // namespace llvm

#endif
