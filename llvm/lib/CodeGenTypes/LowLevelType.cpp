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
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
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

LLT LLT::tensor(LLT ElementType, ArrayRef<int64_t> Shape,
                ArrayRef<int64_t> Strides, TensorDataFormat DataFormat,
                TensorDataType DataType) {
  return tensorImpl(ElementType, Shape.data(),
                    Strides.empty() ? nullptr : Strides.data(), Shape.size(),
                    DataFormat, DataType);
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
