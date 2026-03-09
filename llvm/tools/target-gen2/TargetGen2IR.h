//===- TargetGen2IR.h - CoreDSL LLVM IR emission ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_TARGET_GEN2_IR_H
#define LLVM_TOOLS_TARGET_GEN2_IR_H

#include "TargetGen2AST.h"
#include <string>

namespace llvm {
namespace targetgen2 {

std::string toLLVMIR(const Description &Desc);

} // namespace targetgen2
} // namespace llvm

#endif
