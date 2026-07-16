//===- LowerMemRefIntrinsicInfo.cpp - MemRef intrinsic hooks --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/LowerMemRefIntrinsicInfo.h"

using namespace llvm;
using namespace llvm::memref_lowering;

bool memref_lowering::getIntrinsicOperandRoles(Intrinsic::ID ID, CallInst *CI,
                                               OperandRoles &Roles) {
  (void)ID;
  (void)CI;
  (void)Roles;

  // Fill this in for the real memref intrinsic set.
  //
  // Example:
  //
  // switch (ID) {
  // case Intrinsic::your_memref_elem_add:
  //   Roles.InputArgIndices = {0, 1};
  //   Roles.OutputArgIndices = {2};
  //   return true;
  //
  // case Intrinsic::your_memref_sqrt:
  //   Roles.InputArgIndices = {0};
  //   Roles.OutputArgIndices = {1};
  //   return true;
  //
  // default:
  //   return false;
  // }

  return false;
}

bool memref_lowering::emitVectorIntrinsicSemantics(
    IRBuilder<> &B, Intrinsic::ID ID, ArrayRef<Value *> InputVecs,
    unsigned NumOutputs, SmallVectorImpl<Value *> &OutputVecs) {
  (void)B;
  (void)ID;
  (void)InputVecs;
  (void)NumOutputs;
  (void)OutputVecs;

  // Fill this in for the real memref intrinsic semantics.
  //
  // Example:
  //
  // switch (ID) {
  // case Intrinsic::your_memref_elem_add:
  //   if (InputVecs.size() != 2 || NumOutputs != 1)
  //     return false;
  //   OutputVecs.push_back(
  //       B.CreateFAdd(InputVecs[0], InputVecs[1], "memref.vec.add"));
  //   return true;
  //
  // case Intrinsic::your_memref_elem_sub:
  //   if (InputVecs.size() != 2 || NumOutputs != 1)
  //     return false;
  //   OutputVecs.push_back(
  //       B.CreateFSub(InputVecs[0], InputVecs[1], "memref.vec.sub"));
  //   return true;
  //
  // case Intrinsic::your_memref_elem_mul:
  //   if (InputVecs.size() != 2 || NumOutputs != 1)
  //     return false;
  //   OutputVecs.push_back(
  //       B.CreateFMul(InputVecs[0], InputVecs[1], "memref.vec.mul"));
  //   return true;
  //
  // default:
  //   return false;
  // }

  return false;
}

bool memref_lowering::getReduceAddOperandRole(Intrinsic::ID ID, CallInst *CI,
                                              ReduceAddOperandRole &Role) {
  (void)ID;
  (void)CI;
  (void)Role;

  // Fill this in for the real reduce-add intrinsic.
  //
  // Supported output forms:
  //   1. OutputArgIndex == InvalidArgIndex and the intrinsic returns half.
  //   2. OutputArgIndex names a ptr to a scalar half value.
  //   3. OutputArgIndex names a one-element memref if OutputIsMemRef is true.
  //
  // Example, returning scalar:
  //
  // switch (ID) {
  // case Intrinsic::your_memref_reduce_add:
  //   Role.InputArgIndex = 0;
  //   Role.OutputArgIndex = InvalidArgIndex;
  //   Role.OutputIsMemRef = false;
  //   return true;
  // default:
  //   return false;
  // }
  //
  // Example, storing scalar through ptr:
  //
  // switch (ID) {
  // case Intrinsic::your_memref_reduce_add:
  //   Role.InputArgIndex = 0;
  //   Role.OutputArgIndex = 1;
  //   Role.OutputIsMemRef = false;
  //   return true;
  // default:
  //   return false;
  // }
  //
  // Example, storing scalar through one-element memref:
  //
  // switch (ID) {
  // case Intrinsic::your_memref_reduce_add:
  //   Role.InputArgIndex = 0;
  //   Role.OutputArgIndex = 1;
  //   Role.OutputIsMemRef = true;
  //   return true;
  // default:
  //   return false;
  // }

  return false;
}
