//===- LowerMemRefCommon.cpp - MemRef lowering utilities ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/LowerMemRefCommon.h"

#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/Support/TypeSize.h"

#include <optional>

using namespace llvm;
using namespace llvm::memref_lowering;

namespace {

static constexpr unsigned kMaxVectorElems = 1024;
static constexpr unsigned kFP16Bits = 16;

static bool sameIndexPath(ArrayRef<unsigned> A, ArrayRef<unsigned> B) {
  if (A.size() != B.size())
    return false;

  for (unsigned I = 0; I < A.size(); ++I) {
    if (A[I] != B[I])
      return false;
  }

  return true;
}

static bool isPrefixIndexPath(ArrayRef<unsigned> Prefix,
                              ArrayRef<unsigned> Full) {
  if (Prefix.size() > Full.size())
    return false;

  for (unsigned I = 0; I < Prefix.size(); ++I) {
    if (Prefix[I] != Full[I])
      return false;
  }

  return true;
}

/// Try to recover an aggregate element from a value built by insertvalue.
static Value *getInsertedAggregateElement(Value *Agg, ArrayRef<unsigned> Path,
                                          unsigned Depth = 0) {
  if (!Agg || Path.empty() || Depth > 64)
    return nullptr;

  if (auto *C = dyn_cast<Constant>(Agg)) {
    Constant *Elt = C->getAggregateElement(Path[0]);
    if (!Elt)
      return nullptr;

    if (Path.size() == 1)
      return Elt;

    return getInsertedAggregateElement(Elt, Path.drop_front(), Depth + 1);
  }

  if (auto *IVI = dyn_cast<InsertValueInst>(Agg)) {
    ArrayRef<unsigned> InsertPath = IVI->getIndices();

    if (sameIndexPath(InsertPath, Path))
      return IVI->getInsertedValueOperand();

    if (isPrefixIndexPath(InsertPath, Path)) {
      return getInsertedAggregateElement(IVI->getInsertedValueOperand(),
                                         Path.drop_front(InsertPath.size()),
                                         Depth + 1);
    }

    return getInsertedAggregateElement(IVI->getAggregateOperand(), Path,
                                       Depth + 1);
  }

  return nullptr;
}

static std::optional<uint64_t> getConstUInt(Value *V) {
  auto *CI = dyn_cast_or_null<ConstantInt>(V);
  if (!CI)
    return std::nullopt;

  return CI->getZExtValue();
}

static std::optional<uint64_t>
getConstUIntAggregateElement(Value *Agg, ArrayRef<unsigned> Path) {
  return getConstUInt(getInsertedAggregateElement(Agg, Path));
}

static ArrayType *getArrayFieldType(StructType *STy, unsigned FieldNo) {
  if (!STy || FieldNo >= STy->getNumElements())
    return nullptr;

  return dyn_cast<ArrayType>(STy->getElementType(FieldNo));
}

/// Supported MLIR-style memref descriptor:
///
///   {
///     ptr allocPtr,
///     ptr alignedPtr,
///     index offset,
///     [rank x index] sizes,
///     [rank x index] strides
///   }
static bool getSupportedMemRefRank(Value *V, unsigned &Rank) {
  auto *STy = dyn_cast<StructType>(V->getType());
  if (!STy || STy->getNumElements() != 5)
    return false;

  if (!STy->getElementType(0)->isPointerTy() ||
      !STy->getElementType(1)->isPointerTy() ||
      !STy->getElementType(2)->isIntegerTy())
    return false;

  ArrayType *SizesTy = getArrayFieldType(STy, 3);
  ArrayType *StridesTy = getArrayFieldType(STy, 4);
  if (!SizesTy || !StridesTy)
    return false;

  if (SizesTy->getNumElements() != StridesTy->getNumElements())
    return false;

  if (!SizesTy->getElementType()->isIntegerTy() ||
      !StridesTy->getElementType()->isIntegerTy())
    return false;

  uint64_t Rank64 = SizesTy->getNumElements();
  if (Rank64 != 1 && Rank64 != 2)
    return false;

  Rank = static_cast<unsigned>(Rank64);
  return true;
}

static bool decode1DMemRef(Value *V, unsigned ArgIndex, DecodedMemRef &MR,
                           bool RequireContiguous) {
  auto Size0 = getConstUIntAggregateElement(V, {3, 0});
  auto Stride0 = getConstUIntAggregateElement(V, {4, 0});

  if (!Size0 || !Stride0)
    return false;

  if (*Size0 == 0 || *Size0 > kMaxVectorElems)
    return false;

  if (RequireContiguous && *Stride0 != 1)
    return false;

  MR.MemRefValue = V;
  MR.ArgIndex = ArgIndex;
  MR.Rank = 1;
  MR.Size0 = *Size0;
  MR.Size1 = 1;
  MR.Stride0 = *Stride0;
  MR.Stride1 = 1;
  MR.FlattenDim = 0;
  MR.FlattenStride = *Stride0;
  MR.NumElems = static_cast<unsigned>(*Size0);
  return true;
}

static bool decode2DMemRef(Value *V, unsigned ArgIndex, DecodedMemRef &MR,
                           bool RequireContiguous) {
  auto Size0 = getConstUIntAggregateElement(V, {3, 0});
  auto Size1 = getConstUIntAggregateElement(V, {3, 1});
  auto Stride0 = getConstUIntAggregateElement(V, {4, 0});
  auto Stride1 = getConstUIntAggregateElement(V, {4, 1});

  if (!Size0 || !Size1 || !Stride0 || !Stride1)
    return false;

  if (*Size0 == 0 || *Size1 == 0)
    return false;

  if (*Size0 != 1 && *Size1 != 1)
    return false;

  unsigned FlattenDim = 0;
  uint64_t FlattenStride = 1;
  unsigned NumElems = 0;

  if (*Size0 == 1) {
    if (RequireContiguous && *Stride1 != 1)
      return false;

    if (*Size1 > kMaxVectorElems)
      return false;

    FlattenDim = 1;
    FlattenStride = *Stride1;
    NumElems = static_cast<unsigned>(*Size1);
  } else {
    if (RequireContiguous && *Stride0 != 1)
      return false;

    if (*Size0 > kMaxVectorElems)
      return false;

    FlattenDim = 0;
    FlattenStride = *Stride0;
    NumElems = static_cast<unsigned>(*Size0);
  }

  MR.MemRefValue = V;
  MR.ArgIndex = ArgIndex;
  MR.Rank = 2;
  MR.Size0 = *Size0;
  MR.Size1 = *Size1;
  MR.Stride0 = *Stride0;
  MR.Stride1 = *Stride1;
  MR.FlattenDim = FlattenDim;
  MR.FlattenStride = FlattenStride;
  MR.NumElems = NumElems;
  return true;
}

static bool hasSameLogicalShape(const DecodedMemRef &A,
                                const DecodedMemRef &B) {
  return A.Rank == B.Rank && A.Size0 == B.Size0 && A.Size1 == B.Size1;
}

static bool validateSameShapeAndLength(ArrayRef<DecodedMemRef> MemRefs,
                                       unsigned &NumElems) {
  if (MemRefs.empty())
    return false;

  const DecodedMemRef &First = MemRefs.front();
  if (First.NumElems == 0)
    return false;

  for (const DecodedMemRef &MR : MemRefs) {
    if (!hasSameLogicalShape(First, MR) || MR.NumElems != First.NumElems)
      return false;
  }

  NumElems = First.NumElems;
  return true;
}

static Value *emitExtractValue(IRBuilder<> &B, Value *Agg,
                               ArrayRef<unsigned> Path, const Twine &Name) {
  return B.CreateExtractValue(Agg, Path, Name);
}

static Value *emitUpdatedOffset(IRBuilder<> &B, const DecodedMemRef &MR,
                                unsigned TileStart, const Twine &Name) {
  Value *OldOffset =
      emitExtractValue(B, MR.MemRefValue, {2}, Name + ".old.offset");

  if (TileStart == 0)
    return OldOffset;

  auto *OffsetTy = cast<IntegerType>(OldOffset->getType());
  uint64_t DeltaElems = static_cast<uint64_t>(TileStart) * MR.FlattenStride;
  Value *Delta = ConstantInt::get(OffsetTy, DeltaElems);
  return B.CreateAdd(OldOffset, Delta, Name + ".new.offset");
}

} // namespace

VectorType *memref_lowering::getFixedVectorTy(Type *ElemTy,
                                              unsigned NumElems) {
#if LLVM_VERSION_MAJOR >= 11
  return VectorType::get(ElemTy, NumElems, false);
#else
  return VectorType::get(ElemTy, NumElems);
#endif
}

unsigned
memref_lowering::getTargetFP16VectorLanes(const TargetTransformInfo &TTI) {
#if LLVM_VERSION_MAJOR >= 12
  TypeSize RegBits =
      TTI.getRegisterBitWidth(TargetTransformInfo::RGK_FixedWidthVector);

  if (RegBits.isScalable())
    return 0;

  uint64_t FixedBits = RegBits.getFixedValue();
#else
  unsigned FixedBits =
      TTI.getRegisterBitWidth(TargetTransformInfo::RGK_FixedWidthVector);
#endif

  if (FixedBits < kFP16Bits)
    return 0;

  uint64_t Lanes = FixedBits / kFP16Bits;
  if (Lanes == 0 || Lanes > kMaxVectorElems)
    return 0;

  return static_cast<unsigned>(Lanes);
}

bool memref_lowering::decodeMemRef(Value *V, unsigned ArgIndex,
                                   DecodedMemRef &MR,
                                   bool RequireContiguous) {
  unsigned Rank = 0;
  if (!getSupportedMemRefRank(V, Rank))
    return false;

  if (Rank == 1)
    return decode1DMemRef(V, ArgIndex, MR, RequireContiguous);

  if (Rank == 2)
    return decode2DMemRef(V, ArgIndex, MR, RequireContiguous);

  return false;
}

bool memref_lowering::decodeCallWithOperandRoles(CallInst *CI,
                                                 const OperandRoles &Roles,
                                                 DecodedCall &DC) {
  if (!CI || !CI->getType()->isVoidTy())
    return false;

  Function *Callee = CI->getCalledFunction();
  if (!Callee)
    return false;

  Intrinsic::ID ID = Callee->getIntrinsicID();
  if (ID == Intrinsic::not_intrinsic)
    return false;

  if (Roles.InputArgIndices.empty() || Roles.OutputArgIndices.empty())
    return false;

  DC.CI = CI;
  DC.IntrinsicID = ID;
  DC.ElemTy = Type::getHalfTy(CI->getContext());

  for (unsigned ArgIdx : Roles.InputArgIndices) {
    if (ArgIdx >= CI->arg_size())
      return false;

    DecodedMemRef MR;
    if (!decodeMemRef(CI->getArgOperand(ArgIdx), ArgIdx, MR))
      return false;

    DC.Inputs.push_back(MR);
    DC.AllMemRefs.push_back(MR);
  }

  for (unsigned ArgIdx : Roles.OutputArgIndices) {
    if (ArgIdx >= CI->arg_size())
      return false;

    DecodedMemRef MR;
    if (!decodeMemRef(CI->getArgOperand(ArgIdx), ArgIdx, MR))
      return false;

    DC.Outputs.push_back(MR);
    DC.AllMemRefs.push_back(MR);
  }

  if (!validateSameShapeAndLength(DC.AllMemRefs, DC.NumElems))
    return false;

  return true;
}

Value *memref_lowering::emitAlignedPtr(IRBuilder<> &B,
                                       const DecodedMemRef &MR,
                                       const Twine &Name) {
  return emitExtractValue(B, MR.MemRefValue, {1}, Name);
}

Value *memref_lowering::emitOffset(IRBuilder<> &B, const DecodedMemRef &MR,
                                   const Twine &Name) {
  return emitExtractValue(B, MR.MemRefValue, {2}, Name);
}

Value *memref_lowering::emitBaseOffsetPtr(IRBuilder<> &B, Type *ElemTy,
                                          const DecodedMemRef &MR,
                                          const Twine &Name) {
  Value *AlignedPtr = emitAlignedPtr(B, MR, Name + ".aligned");
  Value *Offset = emitOffset(B, MR, Name + ".offset");

  // The offset is interpreted as an element offset, not a byte offset.
  return B.CreateGEP(ElemTy, AlignedPtr, Offset, Name + ".ptr");
}

Value *memref_lowering::buildTiledMemRef(IRBuilder<> &B,
                                         const DecodedMemRef &MR,
                                         unsigned TileStart,
                                         unsigned TileSize,
                                         const Twine &Name) {
  Value *Tiled = MR.MemRefValue;

  auto *STy = cast<StructType>(MR.MemRefValue->getType());
  auto *SizesTy = cast<ArrayType>(STy->getElementType(3));
  auto *SizeElemTy = cast<IntegerType>(SizesTy->getElementType());

  Value *NewOffset = emitUpdatedOffset(B, MR, TileStart, Name);
  Tiled = B.CreateInsertValue(Tiled, NewOffset, {2}, Name + ".with.offset");

  Value *NewSize = ConstantInt::get(SizeElemTy, TileSize);
  Tiled = B.CreateInsertValue(Tiled, NewSize, {3, MR.FlattenDim},
                              Name + ".with.size");

  return Tiled;
}

void memref_lowering::copyCallProperties(CallInst *NewCI, CallInst *OldCI) {
  NewCI->setCallingConv(OldCI->getCallingConv());
  NewCI->setAttributes(OldCI->getAttributes());
  NewCI->setDebugLoc(OldCI->getDebugLoc());

  if (OldCI->isTailCall())
    NewCI->setTailCallKind(OldCI->getTailCallKind());
}
