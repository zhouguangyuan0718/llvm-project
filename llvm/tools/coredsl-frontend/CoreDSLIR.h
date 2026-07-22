//===-- CoreDSLIR.h - Target-independent CoreDSL IR ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// Defines the target-independent IR produced by the CoreDSL front-end.
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_COREDSL_FRONTEND_COREDSLIR_H
#define LLVM_TOOLS_COREDSL_FRONTEND_COREDSLIR_H

#include <memory>
#include <string>
#include <vector>

namespace llvm::coredsl {

struct SourceLoc {
  unsigned Line = 0;
  unsigned Column = 0;
};

struct Expr {
  enum Kind { Constant, Symbol, Unary, Binary, Ternary, Call, Subscript, Cast };
  Kind K;
  SourceLoc Loc;
  std::string Text;
  std::vector<std::unique_ptr<Expr>> Args;
};

struct Stmt {
  enum Kind { Block, Expression, Declaration, If, While, For };
  Kind K;
  SourceLoc Loc;
  std::string Name;
  std::string Type;
  std::unique_ptr<Expr> Value;
  std::unique_ptr<Expr> Condition;
  std::unique_ptr<Expr> Step;
  std::vector<std::unique_ptr<Stmt>> Children;
};

struct Operand {
  std::string Name;
  std::string Type;
  std::string Attributes;
  SourceLoc Loc;
};

struct EncodingFragment {
  bool IsConstant = false;
  std::string Text;
  unsigned Width = 0;
  SourceLoc Loc;
};

struct Instruction {
  std::string Name;
  SourceLoc Loc;
  std::vector<Operand> Operands;
  std::vector<EncodingFragment> Encoding;
  std::string Assembly;
  std::unique_ptr<Stmt> Behaviour;
};

struct Module {
  std::string Name = "anonymous";
  std::vector<Instruction> Instructions;
};

} // namespace llvm::coredsl

#endif // LLVM_TOOLS_COREDSL_FRONTEND_COREDSLIR_H
