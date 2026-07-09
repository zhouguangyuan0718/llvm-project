//===- LowerMemRefIntrinsicInfo.h - MemRef intrinsic hooks ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_LOWERMEMREFINTRINSICINFO_H
#define LLVM_TRANSFORMS_SCALAR_LOWERMEMREFINTRINSICINFO_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Scalar/LowerMemRefCommon.h"

namespace llvm {
namespace memref_lowering {

bool getIntrinsicOperandRoles(Intrinsic::ID ID, CallInst *CI,
                              OperandRoles &Roles);

bool emitVectorIntrinsicSemantics(IRBuilder<> &B, Intrinsic::ID ID,
                                  ArrayRef<Value *> InputVecs,
                                  unsigned NumOutputs,
                                  SmallVectorImpl<Value *> &OutputVecs);

} // namespace memref_lowering
} // namespace llvm

#endif // LLVM_TRANSFORMS_SCALAR_LOWERMEMREFINTRINSICINFO_H
