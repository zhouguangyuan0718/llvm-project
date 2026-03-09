//===- TargetGen2AST.h - CoreDSL frontend AST ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_TARGET_GEN2_AST_H
#define LLVM_TOOLS_TARGET_GEN2_AST_H

#include <optional>
#include <string>
#include <vector>

namespace llvm {
namespace targetgen2 {

enum class ExprKind {
  Unknown,
  Assignment,
  Conditional,
  Binary,
  Unary,
  Cast,
  Index,
  Call,
  Member,
  Postfix,
  Identifier,
  Literal,
  Group,
};

enum class StatementKind {
  Empty,
  Compound,
  If,
  Switch,
  Case,
  Default,
  While,
  For,
  DoWhile,
  Spawn,
  Continue,
  Break,
  Return,
  Expression,
};

struct EncodingField {
  bool IsBitValue = false;
  std::string Value;
  std::string Name;
  std::string StartBit;
  std::string EndBit;
};

struct Assembly {
  bool IsStructured = false;
  std::string Mnemonic;
  std::string Template;
};

struct Expr {
  ExprKind Kind = ExprKind::Unknown;
  std::string Text;
  std::string Op;
  std::string Value;
  std::vector<Expr> Children;
};

struct Statement {
  StatementKind Kind = StatementKind::Expression;
  std::string Text;
  std::vector<Expr> Expressions;
  std::vector<Statement> Children;
};

struct Instruction {
  std::string Name;
  std::vector<std::string> Attributes;
  std::vector<EncodingField> Encoding;
  std::optional<Assembly> Asm;
  Statement Behavior;
};

struct AlwaysBlock {
  std::string Name;
  std::vector<std::string> Attributes;
  Statement Behavior;
};

struct ISASections {
  std::optional<std::string> ArchitecturalState;
  std::optional<std::string> Functions;
  std::vector<std::string> CommonInstructionAttributes;
  std::vector<Instruction> Instructions;
  std::vector<std::string> CommonAlwaysAttributes;
  std::vector<AlwaysBlock> AlwaysBlocks;
};

struct InstructionSetDef {
  std::string Name;
  std::vector<std::string> Combines;
  std::optional<std::string> Extends;
  ISASections ISA;
};

struct CoreDef {
  std::string Name;
  std::vector<std::string> Provides;
  ISASections ISA;
};

struct Description {
  std::vector<std::string> Imports;
  std::vector<InstructionSetDef> InstructionSets;
  std::vector<CoreDef> Cores;
};

} // namespace targetgen2
} // namespace llvm

#endif
