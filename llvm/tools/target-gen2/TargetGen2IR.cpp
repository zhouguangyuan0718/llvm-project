//===- TargetGen2IR.cpp - CoreDSL LLVM IR emission -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TargetGen2IR.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include <cctype>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace llvm;
using namespace llvm::targetgen2;

namespace {

struct IRFunctionEmitter {
  raw_ostream &OS;
  unsigned ValueID = 0;
  unsigned LabelID = 0;
  std::unordered_map<std::string, std::string> Env;
  bool Terminated = false;
  std::string LastValue = "0";

  std::string nextValue() { return "%v" + std::to_string(ValueID++); }
  std::string nextLabel(StringRef Prefix) {
    return (Prefix.str() + std::to_string(LabelID++));
  }

  static bool parseIntegerLiteral(StringRef S, uint64_t &Out) {
    StringRef T = S.trim();
    if (T.empty())
      return false;
    if (T.contains("'"))
      return false;
    unsigned Base = 10;
    if (T.starts_with("0x") || T.starts_with("0X")) {
      Base = 16;
      T = T.drop_front(2);
    }
    if (T.empty())
      return false;
    Out = 0;
    for (char C : T) {
      if (!std::isxdigit(static_cast<unsigned char>(C)))
        return false;
      unsigned V = std::isdigit(static_cast<unsigned char>(C)) ? C - '0'
                                                               : std::tolower(C) - 'a' + 10;
      if (V >= Base)
        return false;
      Out = Out * Base + V;
    }
    return true;
  }

  std::string emitExpr(const Expr &E) {
    switch (E.Kind) {
    case ExprKind::Identifier: {
      auto It = Env.find(E.Text);
      if (It != Env.end())
        return It->second;
      return "0";
    }
    case ExprKind::Literal: {
      uint64_t V = 0;
      if (parseIntegerLiteral(E.Value.empty() ? E.Text : E.Value, V))
        return std::to_string(V);
      return "0";
    }
    case ExprKind::Binary: {
      if (E.Children.size() < 2)
        return "0";
      std::string L = emitExpr(E.Children[0]);
      std::string R = emitExpr(E.Children[1]);
      std::string Res = nextValue();
      switch (E.Op) {
      case ExprOp::Add:
        OS << "  " << Res << " = add i64 " << L << ", " << R << "\n";
        return Res;
      case ExprOp::Sub:
        OS << "  " << Res << " = sub i64 " << L << ", " << R << "\n";
        return Res;
      case ExprOp::Mul:
        OS << "  " << Res << " = mul i64 " << L << ", " << R << "\n";
        return Res;
      case ExprOp::Div:
        OS << "  " << Res << " = sdiv i64 " << L << ", " << R << "\n";
        return Res;
      case ExprOp::Eq:
        OS << "  " << Res << " = icmp eq i64 " << L << ", " << R << "\n";
        return Res;
      case ExprOp::Ne:
        OS << "  " << Res << " = icmp ne i64 " << L << ", " << R << "\n";
        return Res;
      case ExprOp::Lt:
        OS << "  " << Res << " = icmp slt i64 " << L << ", " << R << "\n";
        return Res;
      case ExprOp::Le:
        OS << "  " << Res << " = icmp sle i64 " << L << ", " << R << "\n";
        return Res;
      case ExprOp::Gt:
        OS << "  " << Res << " = icmp sgt i64 " << L << ", " << R << "\n";
        return Res;
      case ExprOp::Ge:
        OS << "  " << Res << " = icmp sge i64 " << L << ", " << R << "\n";
        return Res;
      default:
        return "0";
      }
    }
    case ExprKind::Assignment: {
      if (E.Children.size() < 2)
        return "0";
      const Expr &LHS = E.Children[0];
      std::string RHS = emitExpr(E.Children[1]);
      if (LHS.Kind == ExprKind::Identifier)
        Env[LHS.Text] = RHS;
      LastValue = RHS;
      return RHS;
    }
    case ExprKind::Group:
    case ExprKind::Cast:
      if (!E.Children.empty())
        return emitExpr(E.Children.back());
      return "0";
    default:
      return "0";
    }
  }

  std::string emitCondition(const Expr &E) {
    std::string V = emitExpr(E);
    if (!V.empty() && V[0] == '%')
      return V;
    std::string Cmp = nextValue();
    OS << "  " << Cmp << " = icmp ne i64 " << V << ", 0\n";
    return Cmp;
  }

  void emitStmt(const Statement &S) {
    if (Terminated)
      return;
    switch (S.Kind) {
    case StatementKind::Compound:
      for (const Statement &C : S.Children)
        emitStmt(C);
      return;
    case StatementKind::Expression:
      for (const Expr &E : S.Expressions)
        LastValue = emitExpr(E);
      return;
    case StatementKind::Return: {
      std::string RV = "0";
      if (!S.Expressions.empty())
        RV = emitExpr(S.Expressions.front());
      OS << "  ret i64 " << RV << "\n";
      Terminated = true;
      return;
    }
    case StatementKind::If: {
      if (S.Expressions.empty() || S.Children.empty())
        return;
      std::string ThenL = nextLabel("if.then.");
      std::string ElseL = nextLabel("if.else.");
      std::string EndL = nextLabel("if.end.");
      std::string Cond = emitCondition(S.Expressions.front());
      OS << "  br i1 " << Cond << ", label %" << ThenL << ", label %" << ElseL << "\n";
      OS << ThenL << ":\n";
      emitStmt(S.Children[0]);
      if (!Terminated)
        OS << "  br label %" << EndL << "\n";
      bool ThenTerm = Terminated;
      Terminated = false;
      OS << ElseL << ":\n";
      if (S.Children.size() > 1)
        emitStmt(S.Children[1]);
      if (!Terminated)
        OS << "  br label %" << EndL << "\n";
      bool ElseTerm = Terminated;
      Terminated = ThenTerm && ElseTerm;
      if (!Terminated)
        OS << EndL << ":\n";
      return;
    }
    default:
      return;
    }
  }
};

static void collectIdentifiers(const Expr &E,
                               std::vector<std::string> &UsedOrder,
                               std::unordered_set<std::string> &UsedSet,
                               std::unordered_set<std::string> &Assigned) {
  if (E.Kind == ExprKind::Identifier && UsedSet.insert(E.Text).second)
    UsedOrder.push_back(E.Text);
  if (E.Kind == ExprKind::Assignment && E.Children.size() >= 2 &&
      E.Children[0].Kind == ExprKind::Identifier)
    Assigned.insert(E.Children[0].Text);
  for (const Expr &C : E.Children)
    collectIdentifiers(C, UsedOrder, UsedSet, Assigned);
}

static void collectIdentifiers(const Statement &S,
                               std::vector<std::string> &UsedOrder,
                               std::unordered_set<std::string> &UsedSet,
                               std::unordered_set<std::string> &Assigned) {
  for (const Expr &E : S.Expressions)
    collectIdentifiers(E, UsedOrder, UsedSet, Assigned);
  for (const Statement &C : S.Children)
    collectIdentifiers(C, UsedOrder, UsedSet, Assigned);
}

static std::vector<std::string> inferParams(const Instruction &Inst) {
  std::vector<std::string> UsedOrder;
  std::unordered_set<std::string> UsedSet;
  std::unordered_set<std::string> Assigned;
  collectIdentifiers(Inst.Behavior, UsedOrder, UsedSet, Assigned);

  std::vector<std::string> Params;
  std::unordered_set<std::string> Seen;
  for (const std::string &U : UsedOrder) {
    if (!Assigned.count(U) && Seen.insert(U).second)
      Params.push_back(U);
  }
  if (!Params.empty())
    return Params;

  for (const EncodingField &F : Inst.Encoding) {
    if (F.IsBitValue || F.Name.empty())
      continue;
    if (Seen.insert(F.Name).second)
      Params.push_back(F.Name);
  }
  return Params;
}

static void emitInstructionFunction(raw_ostream &OS, StringRef Scope,
                                    const Instruction &Inst) {
  std::vector<std::string> Params = inferParams(Inst);
  OS << "define i64 @tg2.exec." << Scope << "." << Inst.Name << "(";
  for (size_t I = 0; I < Params.size(); ++I) {
    if (I)
      OS << ", ";
    OS << "i64 %" << Params[I];
  }
  OS << ") {\nentry:\n";

  IRFunctionEmitter E{OS};
  for (const std::string &P : Params)
    E.Env[P] = ("%" + P);

  E.emitStmt(Inst.Behavior);
  if (!E.Terminated)
    OS << "  ret i64 " << E.LastValue << "\n";
  OS << "}\n\n";
}

} // namespace

std::string llvm::targetgen2::toLLVMIR(const Description &Desc) {
  std::string Out;
  raw_string_ostream OS(Out);
  OS << "; ModuleID = 'target-gen2'\n";
  OS << "source_filename = \"target-gen2\"\n\n";

  for (const InstructionSetDef &IS : Desc.InstructionSets)
    for (const Instruction &Inst : IS.ISA.Instructions)
      emitInstructionFunction(OS, "inst." + IS.Name, Inst);

  for (const CoreDef &C : Desc.Cores)
    for (const Instruction &Inst : C.ISA.Instructions)
      emitInstructionFunction(OS, "core." + C.Name, Inst);

  return Out;
}
