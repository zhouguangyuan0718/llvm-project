//===- LowerMemRefCommon.h - MemRef lowering utilities ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_LOWERMEMREFCOMMON_H
#define LLVM_TRANSFORMS_SCALAR_LOWERMEMREFCOMMON_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include <cstdint>

namespace llvm {

class TargetTransformInfo;
class Type;
class VectorType;
class Value;

namespace memref_lowering {

struct OperandRoles {
  SmallVector<unsigned, 4> InputArgIndices;
  SmallVector<unsigned, 2> OutputArgIndices;
};

struct DecodedMemRef {
  Value *MemRefValue = nullptr;
  unsigned ArgIndex = 0;

  unsigned Rank = 0;

  uint64_t Size0 = 0;
  uint64_t Size1 = 1;

  uint64_t Stride0 = 0;
  uint64_t Stride1 = 1;

  unsigned FlattenDim = 0;
  uint64_t FlattenStride = 1;

  unsigned NumElems = 0;
};

struct DecodedCall {
  CallInst *CI = nullptr;
  Intrinsic::ID IntrinsicID = Intrinsic::not_intrinsic;

  SmallVector<DecodedMemRef, 4> Inputs;
  SmallVector<DecodedMemRef, 2> Outputs;
  SmallVector<DecodedMemRef, 8> AllMemRefs;

  Type *ElemTy = nullptr;
  unsigned NumElems = 0;
};

VectorType *getFixedVectorTy(Type *ElemTy, unsigned NumElems);

unsigned getTargetFP16VectorLanes(const TargetTransformInfo &TTI);

bool decodeMemRef(Value *V, unsigned ArgIndex, DecodedMemRef &MR);

bool decodeCallWithOperandRoles(CallInst *CI, const OperandRoles &Roles,
                                DecodedCall &DC);

Value *emitAlignedPtr(IRBuilder<> &B, const DecodedMemRef &MR,
                      const Twine &Name);

Value *emitOffset(IRBuilder<> &B, const DecodedMemRef &MR,
                  const Twine &Name);

Value *emitBaseOffsetPtr(IRBuilder<> &B, Type *ElemTy,
                         const DecodedMemRef &MR, const Twine &Name);

Value *buildTiledMemRef(IRBuilder<> &B, const DecodedMemRef &MR,
                        unsigned TileStart, unsigned TileSize,
                        const Twine &Name);

void copyCallProperties(CallInst *NewCI, CallInst *OldCI);

} // namespace memref_lowering
} // namespace llvm

#endif // LLVM_TRANSFORMS_SCALAR_LOWERMEMREFCOMMON_H
