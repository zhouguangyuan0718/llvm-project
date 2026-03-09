//===- TargetGen2JSON.h - CoreDSL JSON emission ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_TARGET_GEN2_JSON_H
#define LLVM_TOOLS_TARGET_GEN2_JSON_H

#include "TargetGen2AST.h"
#include "llvm/Support/JSON.h"

namespace llvm {
namespace targetgen2 {

json::Value toJSON(const Description &D);

} // namespace targetgen2
} // namespace llvm

#endif
