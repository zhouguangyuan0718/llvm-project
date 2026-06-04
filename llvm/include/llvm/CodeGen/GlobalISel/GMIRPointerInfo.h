//===- GMIRPointerInfo.h - GMIR pointer expression analysis -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_GLOBALISEL_GMIRPOINTERINFO_H
#define LLVM_CODEGEN_GLOBALISEL_GMIRPOINTERINFO_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/Register.h"
#include "llvm/Support/Compiler.h"

#include <cstdint>
#include <optional>
#include <utility>

namespace llvm {

class MachineRegisterInfo;
class raw_ostream;

namespace GISelAddressing {

/// Describes an integer offset expression in the canonical form:
///
///   Sum(Reg_i * Scale_i) + Constant
///
/// Terms are sorted by register number. Terms using the same register are
/// merged into a single term.
///
/// All constants, scales, and intermediate results are expected to fit in an
/// int64_t.
class LLVM_ABI GMIRLinearOffset {
public:
  struct Term {
    Register Reg;
    int64_t Scale = 0;

    bool operator==(const Term &Other) const {
      return Reg == Other.Reg && Scale == Other.Scale;
    }

    bool operator!=(const Term &Other) const { return !(*this == Other); }
  };

private:
  SmallVector<Term, 2> Terms;
  int64_t Constant = 0;

public:
  GMIRLinearOffset() = default;

  static GMIRLinearOffset getConstant(int64_t Value);
  static GMIRLinearOffset getReg(Register Reg, int64_t Scale = 1);

  bool operator==(const GMIRLinearOffset &Other) const {
    return Constant == Other.Constant && Terms == Other.Terms;
  }

  bool operator!=(const GMIRLinearOffset &Other) const {
    return !(*this == Other);
  }

  ArrayRef<Term> getTerms() const { return Terms; }
  int64_t getConstant() const { return Constant; }

  bool isZero() const { return Terms.empty() && Constant == 0; }
  bool isConstant() const { return Terms.empty(); }
  bool hasVariablePart() const { return !Terms.empty(); }

  /// Add a constant value to this expression.
  void addConstant(int64_t Value);

  /// Add Reg * Scale to this expression.
  void addTerm(Register Reg, int64_t Scale = 1);

  /// Add another linear expression to this expression.
  void add(const GMIRLinearOffset &Other);

  /// Subtract another linear expression from this expression.
  void subtract(const GMIRLinearOffset &Other);

  /// Multiply the whole expression by a constant.
  void multiply(int64_t Scale);

  /// Returns true if both expressions contain exactly the same register terms.
  bool hasSameVariablePart(const GMIRLinearOffset &Other) const {
    return Terms == Other.Terms;
  }

  /// If both expressions have the same register terms, return:
  ///
  ///   Other - *this
  ///
  /// Otherwise return std::nullopt.
  std::optional<int64_t>
  getConstantDifference(const GMIRLinearOffset &Other) const;
};

/// Describes a GMIR pointer in the canonical form:
///
///   BaseReg + Offset
///
/// BaseReg is an opaque pointer-producing register. Offset is a canonical
/// linear integer expression.
class LLVM_ABI GMIRPointerInfo {
  Register BaseReg;
  GMIRLinearOffset Offset;

public:
  GMIRPointerInfo() = default;

  explicit GMIRPointerInfo(Register BaseReg) : BaseReg(BaseReg) {}

  GMIRPointerInfo(Register BaseReg, GMIRLinearOffset Offset)
      : BaseReg(BaseReg), Offset(std::move(Offset)) {}

  bool operator==(const GMIRPointerInfo &Other) const {
    return BaseReg == Other.BaseReg && Offset == Other.Offset;
  }

  bool operator!=(const GMIRPointerInfo &Other) const {
    return !(*this == Other);
  }

  bool isValid() const { return BaseReg.isValid(); }

  Register getBase() const { return BaseReg; }
  const GMIRLinearOffset &getOffset() const { return Offset; }

  /// Print this pointer description to OS.
  void print(raw_ostream &OS) const;

  /// Dump this pointer description to dbgs().
  LLVM_DUMP_METHOD void dump() const;

  /// Construct a new pointer description by adding ExtraOffset.
  GMIRPointerInfo withAddedOffset(const GMIRLinearOffset &ExtraOffset) const;

  /// Construct a new pointer description by adding a constant offset.
  GMIRPointerInfo withAddedConstant(int64_t ExtraOffset) const;

  /// Construct a new pointer description by adding Reg * Scale.
  GMIRPointerInfo withAddedReg(Register Reg, int64_t Scale = 1) const;

  /// If both pointers have the same base and the same variable offset terms,
  /// return:
  ///
  ///   Other - *this
  ///
  /// Otherwise return std::nullopt.
  std::optional<int64_t>
  getConstantDifference(const GMIRPointerInfo &Other) const;
};

/// Analyzes GMIR pointer and integer offset expressions.
///
/// This initial implementation requires SSA-form MachineIR.
class LLVM_ABI GMIRPointerAnalyzer {
  MachineRegisterInfo &MRI;
  unsigned MaxDepth;

public:
  explicit GMIRPointerAnalyzer(MachineRegisterInfo &MRI,
                               unsigned MaxDepth = 32);

  /// Analyze a pointer-producing register.
  GMIRPointerInfo getPointerInfo(Register Ptr) const;

  /// Analyze an integer register as a linear offset expression.
  GMIRLinearOffset getOffsetInfo(Register OffsetReg) const;
};

} // namespace GISelAddressing
} // namespace llvm

#endif // LLVM_CODEGEN_GLOBALISEL_GMIRPOINTERINFO_H
