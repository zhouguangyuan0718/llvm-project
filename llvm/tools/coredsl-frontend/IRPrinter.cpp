//===-- IRPrinter.cpp - CDSLIR textual printer --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "IRPrinter.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::coredsl;

namespace {

static void printIndent(raw_ostream &OS, unsigned Depth) {
  OS.indent(Depth * 2);
}

static void printExpr(raw_ostream &OS, const Expr &E) {
  switch (E.K) {
  case Expr::Constant:
    OS << "const " << E.Text;
    return;
  case Expr::Symbol:
    OS << "symbol @" << E.Text;
    return;
  case Expr::Unary:
  case Expr::Binary:
  case Expr::Ternary:
  case Expr::Cast:
    OS << (E.K == Expr::Unary     ? "unary "
           : E.K == Expr::Binary  ? "binary "
           : E.K == Expr::Ternary ? "ternary "
                                  : "cast ")
       << '"' << E.Text << '"' << '(';
    break;
  case Expr::Call:
    OS << "call(";
    break;
  case Expr::Subscript:
    OS << "subscript(";
    break;
  }
  llvm::interleaveComma(E.Args, OS, [&](const std::unique_ptr<Expr> &Arg) {
    printExpr(OS, *Arg);
  });
  OS << ')';
}

static void printStmt(raw_ostream &OS, const Stmt &S, unsigned Depth) {
  printIndent(OS, Depth);
  switch (S.K) {
  case Stmt::Block:
    OS << "block {\n";
    for (const auto &Child : S.Children)
      printStmt(OS, *Child, Depth + 1);
    printIndent(OS, Depth);
    OS << "}\n";
    return;
  case Stmt::Expression:
    OS << "expr ";
    printExpr(OS, *S.Value);
    OS << '\n';
    return;
  case Stmt::Declaration:
    OS << "let @" << S.Name << " : " << S.Type;
    if (S.Value) {
      OS << " = ";
      printExpr(OS, *S.Value);
    }
    OS << '\n';
    return;
  case Stmt::If:
    OS << "if ";
    printExpr(OS, *S.Condition);
    OS << " {\n";
    printStmt(OS, *S.Children[0], Depth + 1);
    printIndent(OS, Depth);
    OS << '}';
    if (S.Children.size() == 2) {
      OS << " else {\n";
      printStmt(OS, *S.Children[1], Depth + 1);
      printIndent(OS, Depth);
      OS << '}';
    }
    OS << '\n';
    return;
  case Stmt::While:
    OS << "while ";
    printExpr(OS, *S.Condition);
    OS << " {\n";
    printStmt(OS, *S.Children[0], Depth + 1);
    printIndent(OS, Depth);
    OS << "}\n";
    return;
  case Stmt::For:
    OS << "for";
    if (S.Condition) {
      OS << " condition ";
      printExpr(OS, *S.Condition);
    }
    if (S.Step) {
      OS << " step ";
      printExpr(OS, *S.Step);
    }
    OS << " {\n";
    for (const auto &Child : S.Children)
      printStmt(OS, *Child, Depth + 1);
    printIndent(OS, Depth);
    OS << "}\n";
    return;
  }
}

} // namespace

void llvm::coredsl::printIR(raw_ostream &OS, const Module &M) {
  OS << "coredsl.module @" << M.Name << " {\n";
  for (const Instruction &Inst : M.Instructions) {
    printIndent(OS, 1);
    OS << "coredsl.instruction @" << Inst.Name << " {\n";
    for (const Operand &Op : Inst.Operands) {
      printIndent(OS, 2);
      OS << "operand @" << Op.Name << " : " << Op.Type;
      if (!Op.Attributes.empty())
        OS << " attributes [" << Op.Attributes << ']';
      OS << '\n';
    }
    printIndent(OS, 2);
    OS << "encoding ";
    unsigned Width = 0;
    for (const EncodingFragment &Fragment : Inst.Encoding)
      Width += Fragment.Width;
    OS << "width " << Width << " [";
    interleaveComma(Inst.Encoding, OS, [&](const EncodingFragment &Fragment) {
      OS << (Fragment.IsConstant ? "const " : "field ") << Fragment.Text;
    });
    OS << "]\n";
    if (!Inst.Assembly.empty()) {
      printIndent(OS, 2);
      OS << "assembly " << Inst.Assembly << '\n';
    }
    printIndent(OS, 2);
    OS << "behavior {\n";
    printStmt(OS, *Inst.Behaviour, 3);
    printIndent(OS, 2);
    OS << "}\n";
    printIndent(OS, 1);
    OS << "}\n";
  }
  OS << "}\n";
}
