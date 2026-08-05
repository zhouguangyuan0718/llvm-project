//===-- llvm/CodeGenTypes/LowLevelType.cpp
//---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file This file implements the more header-heavy bits of the LLT class to
/// avoid polluting users' namespaces.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGenTypes/LowLevelType.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <limits>
using namespace llvm;

bool LLT::ExtendedLLT = false;

static StringRef getTensorDataTypeName(TensorDataType DataType) {
  switch (DataType) {
  case TensorDataType::Invalid:
    return "invalid";
  case TensorDataType::Bool:
    return "bool";
  case TensorDataType::SInt:
    return "sint";
  case TensorDataType::UInt:
    return "uint";
  case TensorDataType::IEEEFloat:
    return "ieeefloat";
  case TensorDataType::BFloat:
    return "bfloat";
  case TensorDataType::TF32:
    return "tf32";
  case TensorDataType::Float8E4M3FN:
    return "f8e4m3fn";
  case TensorDataType::Float8E5M2:
    return "f8e5m2";
  }
  llvm_unreachable("unknown tensor data type");
}

static StringRef getTensorDataFormatName(TensorDataFormat DataFormat) {
  switch (DataFormat) {
  case TensorDataFormat::GenericStrided:
    return "generic";
  case TensorDataFormat::NCHW:
    return "nchw";
  case TensorDataFormat::NHWC:
    return "nhwc";
  case TensorDataFormat::NCDHW:
    return "ncdhw";
  case TensorDataFormat::NDHWC:
    return "ndhwc";
  }
  llvm_unreachable("unknown tensor data format");
}

static Error invalidTensor(const Twine &Message) {
  return createStringError(inconvertibleErrorCode(), Message);
}

static bool checkedMultiply(uint64_t LHS, uint64_t RHS, uint64_t &Result) {
  if (LHS != 0 && RHS > std::numeric_limits<uint64_t>::max() / LHS)
    return true;
  Result = LHS * RHS;
  return false;
}

static bool checkedAdd(uint64_t LHS, uint64_t RHS, uint64_t &Result) {
  if (RHS > std::numeric_limits<uint64_t>::max() - LHS)
    return true;
  Result = LHS + RHS;
  return false;
}

static Error validateDataFormat(TensorDataFormat Format, size_t Rank) {
  if (static_cast<uint16_t>(Format) >
      static_cast<uint16_t>(TensorDataFormat::NDHWC))
    return invalidTensor("unknown tensor data format");

  switch (Format) {
  case TensorDataFormat::NCHW:
  case TensorDataFormat::NHWC:
    if (Rank != 4)
      return invalidTensor("NCHW and NHWC tensor formats require rank 4");
    break;
  case TensorDataFormat::NCDHW:
  case TensorDataFormat::NDHWC:
    if (Rank != 5)
      return invalidTensor("NCDHW and NDHWC tensor formats require rank 5");
    break;
  case TensorDataFormat::GenericStrided:
    break;
  }
  return Error::success();
}

static Error validateDataType(LLT ElementType, TensorDataType DataType) {
  if (static_cast<uint16_t>(DataType) >
      static_cast<uint16_t>(TensorDataType::Float8E5M2))
    return invalidTensor("unknown tensor data type");

  unsigned BitWidth = ElementType.getScalarSizeInBits();
  bool IsIntegerRepresentation =
      ElementType.isAnyScalar() || ElementType.isInteger();
  bool IsFloatRepresentation =
      ElementType.isAnyScalar() || ElementType.isFloat();

  switch (DataType) {
  case TensorDataType::Invalid:
    return invalidTensor("tensor data type cannot be invalid");
  case TensorDataType::Bool:
    if (!IsIntegerRepresentation || BitWidth != 1)
      return invalidTensor(
          "boolean tensor elements require a 1-bit integer representation");
    break;
  case TensorDataType::SInt:
  case TensorDataType::UInt:
    if (!IsIntegerRepresentation)
      return invalidTensor(
          "integer tensor data types require an integer representation");
    break;
  case TensorDataType::IEEEFloat:
    if (!IsFloatRepresentation)
      return invalidTensor(
          "IEEE floating tensor data requires a floating representation");
    break;
  case TensorDataType::BFloat:
    if (!IsFloatRepresentation || BitWidth != 16)
      return invalidTensor(
          "bfloat tensor elements require a 16-bit representation");
    break;
  case TensorDataType::TF32:
    if (!IsFloatRepresentation || BitWidth != 32)
      return invalidTensor(
          "TF32 tensor elements require a 32-bit representation");
    break;
  case TensorDataType::Float8E4M3FN:
  case TensorDataType::Float8E5M2:
    if (!IsFloatRepresentation || BitWidth != 8)
      return invalidTensor(
          "float8 tensor elements require an 8-bit representation");
    break;
  }
  return Error::success();
}

Expected<LLT> LLT::tensor(LLT ElementType, ArrayRef<int64_t> Shape,
                          ArrayRef<int64_t> Strides,
                          TensorDataFormat DataFormat,
                          TensorDataType DataType) {
  if (!ElementType.isValid() || !ElementType.isScalar() ||
      ElementType.isToken())
    return invalidTensor("tensor element type must be a non-token scalar LLT");
  if (Shape.empty() || Shape.size() > MaxTensorRank)
    return invalidTensor("tensor rank must be between 1 and 5");
  if (!Strides.empty() && Strides.size() != Shape.size())
    return invalidTensor("tensor shape and stride ranks must match");
  if (Error E = validateDataFormat(DataFormat, Shape.size()))
    return std::move(E);
  if (Error E = validateDataType(ElementType, DataType))
    return std::move(E);

  uint64_t ElementCount = 1;
  for (int64_t Dim : Shape) {
    if (Dim <= 0)
      return invalidTensor("tensor dimensions must be positive");
    if (checkedMultiply(ElementCount, static_cast<uint64_t>(Dim), ElementCount))
      return invalidTensor("tensor element count overflows 64 bits");
  }

  SmallVector<int64_t, MaxTensorRank> CanonicalStrides;
  if (Strides.empty()) {
    CanonicalStrides.resize(Shape.size());
    uint64_t RunningStride = 1;
    for (size_t I = Shape.size(); I != 0; --I) {
      if (RunningStride >
          static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        return invalidTensor("tensor stride does not fit in 64 signed bits");
      CanonicalStrides[I - 1] = static_cast<int64_t>(RunningStride);
      if (checkedMultiply(RunningStride, static_cast<uint64_t>(Shape[I - 1]),
                          RunningStride))
        return invalidTensor("tensor stride computation overflows 64 bits");
    }
    Strides = CanonicalStrides;
  } else {
    for (int64_t Stride : Strides)
      if (Stride <= 0)
        return invalidTensor("tensor strides must be positive");
  }

  uint64_t MaxOffset = 0;
  for (size_t I = 0; I != Shape.size(); ++I) {
    uint64_t AxisOffset;
    if (checkedMultiply(static_cast<uint64_t>(Shape[I] - 1),
                        static_cast<uint64_t>(Strides[I]), AxisOffset) ||
        checkedAdd(MaxOffset, AxisOffset, MaxOffset))
      return invalidTensor("tensor storage span overflows 64 bits");
  }

  uint64_t StorageElements;
  uint64_t LogicalSizeInBits;
  uint64_t StorageSizeInBits;
  uint64_t ElementBits = ElementType.getScalarSizeInBits();
  if (checkedAdd(MaxOffset, 1, StorageElements) ||
      checkedMultiply(ElementCount, ElementBits, LogicalSizeInBits) ||
      checkedMultiply(StorageElements, ElementBits, StorageSizeInBits))
    return invalidTensor("tensor size in bits overflows 64 bits");

  LLT Tensor;
  Tensor.Info = toTensor(ElementType.Info);
  Tensor.RawData =
      ElementType.RawData | maskAndShift(Shape.size(), TensorRankFieldInfo) |
      maskAndShift(static_cast<uint16_t>(DataFormat),
                   TensorDataFormatFieldInfo) |
      maskAndShift(static_cast<uint16_t>(DataType), TensorDataTypeFieldInfo);
  Tensor.TensorElementCount = ElementCount;
  std::copy(Shape.begin(), Shape.end(), Tensor.TensorShape.begin());
  std::copy(Strides.begin(), Strides.end(), Tensor.TensorStrides.begin());
  return Tensor;
}

static LLT::FpSemantics getFpSemanticsForMVT(MVT VT) {
  switch (VT.getScalarType().SimpleTy) {
  default:
    llvm_unreachable("Unknown FP format");
  case MVT::f16:
    return LLT::FpSemantics::S_IEEEhalf;
  case MVT::bf16:
    return LLT::FpSemantics::S_BFloat;
  case MVT::f32:
    return LLT::FpSemantics::S_IEEEsingle;
  case MVT::f64:
    return LLT::FpSemantics::S_IEEEdouble;
  case MVT::f80:
    return LLT::FpSemantics::S_x87DoubleExtended;
  case MVT::f128:
    return LLT::FpSemantics::S_IEEEquad;
  case MVT::ppcf128:
    return LLT::FpSemantics::S_PPCDoubleDouble;
  }
}

LLT::LLT(MVT VT) {
  if (!ExtendedLLT) {
    if (VT.isVector()) {
      bool AsVector = VT.getVectorMinNumElements() > 1 || VT.isScalableVector();
      Kind Info = AsVector ? Kind::VECTOR_ANY : Kind::ANY_SCALAR;
      init(Info, VT.getVectorElementCount(),
           VT.getVectorElementType().getSizeInBits());
    } else if (VT.isValid() && !VT.isScalableTargetExtVT()) {
      init(Kind::ANY_SCALAR, ElementCount::getFixed(0), VT.getSizeInBits());
    } else {
      this->Info = Kind::INVALID;
      this->RawData = 0;
    }
    return;
  }

  bool IsFloatingPoint = VT.isFloatingPoint();
  bool AsVector = VT.isVector() &&
                  (VT.getVectorMinNumElements() > 1 || VT.isScalableVector());

  if (AsVector) {
    if (IsFloatingPoint)
      init(LLT::Kind::VECTOR_FLOAT, VT.getVectorElementCount(),
           VT.getVectorElementType().getSizeInBits(), getFpSemanticsForMVT(VT));
    else
      init(LLT::Kind::VECTOR_INTEGER, VT.getVectorElementCount(),
           VT.getVectorElementType().getSizeInBits());
  } else if (VT.isValid() && !VT.isScalableTargetExtVT()) {
    // Aggregates are no different from real scalars as far as GlobalISel is
    // concerned.
    if (IsFloatingPoint)
      init(LLT::Kind::FLOAT, ElementCount::getFixed(0), VT.getSizeInBits(),
           getFpSemanticsForMVT(VT));
    else
      init(LLT::Kind::INTEGER, ElementCount::getFixed(0), VT.getSizeInBits());
  } else {
    this->Info = Kind::INVALID;
    this->RawData = 0;
  }
  return;
}

TypeSize LLT::getTensorLogicalSizeInBits() const {
  assert(isTensor() && "expected a tensor LLT");
  return TypeSize::getFixed(TensorElementCount *
                            getTensorElementType().getScalarSizeInBits());
}

TypeSize LLT::getTensorStorageSizeInBits() const {
  assert(isTensor() && "expected a tensor LLT");
  uint64_t MaxOffset = 0;
  ArrayRef<int64_t> Shape = getTensorShape();
  ArrayRef<int64_t> Strides = getTensorStrides();
  for (unsigned I = 0; I != Shape.size(); ++I)
    MaxOffset +=
        static_cast<uint64_t>(Shape[I] - 1) * static_cast<uint64_t>(Strides[I]);
  return TypeSize::getFixed((MaxOffset + 1) *
                            getTensorElementType().getScalarSizeInBits());
}

void LLT::Profile(FoldingSetNodeID &ID) const {
  if (!isTensor()) {
    ID.AddInteger(getUniqueRAWLLTData());
    return;
  }

  ID.AddInteger(static_cast<uint8_t>(Info));
  ID.AddInteger(getTensorElementType().getScalarSizeInBits());
  if (isFloatTensor())
    ID.AddInteger(
        static_cast<uint8_t>(getTensorElementType().getFpSemantics()));
  ID.AddInteger(TensorElementCount);
  ID.AddInteger(getTensorShape().size());
  ID.AddInteger(static_cast<uint16_t>(getTensorDataFormat()));
  ID.AddInteger(static_cast<uint16_t>(getTensorDataType()));
  for (unsigned I = 0; I != getTensorShape().size(); ++I) {
    ID.AddInteger(TensorShape[I]);
    ID.AddInteger(TensorStrides[I]);
  }
}

unsigned LLT::getHashValue() const {
  if (!isTensor())
    return DenseMapInfo<uint64_t>::getHashValue(getUniqueRAWLLTData());
  FoldingSetNodeID ID;
  Profile(ID);
  return ID.ComputeHash();
}

void LLT::print(raw_ostream &OS) const {
  if (isTensor()) {
    OS << "tensor<elem=" << getTensorElementType()
       << ",dtype=" << getTensorDataTypeName(getTensorDataType()) << ",shape=[";
    interleaveComma(getTensorShape(), OS);
    OS << "],strides=[";
    interleaveComma(getTensorStrides(), OS);
    OS << "],format=" << getTensorDataFormatName(getTensorDataFormat()) << ">";
  } else if (isVector()) {
    OS << "<";
    OS << getElementCount() << " x " << getElementType() << ">";
  } else if (isPointer()) {
    OS << "p" << getAddressSpace();
  } else if (isBFloat16()) {
    OS << "bf16";
  } else if (isPPCF128()) {
    OS << "ppcf128";
  } else if (isFloatIEEE()) {
    OS << "f" << getScalarSizeInBits();
  } else if (isInteger()) {
    OS << "i" << getScalarSizeInBits();
  } else if (isValid()) {
    assert(isScalar() && "unexpected type");
    OS << "s" << getScalarSizeInBits();
  } else {
    OS << "LLT_invalid";
  }
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void LLT::dump() const {
  print(dbgs());
  dbgs() << '\n';
}
#endif
