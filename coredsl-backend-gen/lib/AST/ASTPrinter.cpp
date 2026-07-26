#include "coredsl/ASTPrinter.h"

#include <string>

namespace coredsl {

namespace {

void indent(llvm::raw_ostream &OS, unsigned Depth) {
  for (unsigned I = 0; I < Depth; ++I)
    OS << "  ";
}

void printExpr(const Expr &Expression, llvm::raw_ostream &OS) {
  switch (Expression.kind()) {
  case Expr::Kind::Identifier:
    OS << static_cast<const IdentifierExpr &>(Expression).Name;
    return;
  case Expr::Kind::Integer:
  case Expr::Kind::String:
    OS << static_cast<const LiteralExpr &>(Expression).Value;
    return;
  case Expr::Kind::Dynamic:
    OS << '?';
    return;
  case Expr::Kind::Unary: {
    const auto &Unary = static_cast<const UnaryExpr &>(Expression);
    OS << '(' << (Unary.IsPostfix ? "post" : "unary") << ' ' << Unary.Operator
       << ' ';
    printExpr(*Unary.Operand, OS);
    OS << ')';
    return;
  }
  case Expr::Kind::Binary: {
    const auto &Binary = static_cast<const BinaryExpr &>(Expression);
    OS << '(' << Binary.Operator << ' ';
    printExpr(*Binary.LHS, OS);
    OS << ' ';
    printExpr(*Binary.RHS, OS);
    OS << ')';
    return;
  }
  case Expr::Kind::Conditional: {
    const auto &Conditional = static_cast<const ConditionalExpr &>(Expression);
    OS << "(?: ";
    printExpr(*Conditional.Condition, OS);
    OS << ' ';
    printExpr(*Conditional.TrueExpr, OS);
    OS << ' ';
    printExpr(*Conditional.FalseExpr, OS);
    OS << ')';
    return;
  }
  case Expr::Kind::Index: {
    const auto &Index = static_cast<const IndexExpr &>(Expression);
    OS << "(index ";
    printExpr(*Index.Base, OS);
    OS << ' ';
    printExpr(*Index.Index, OS);
    OS << ')';
    return;
  }
  case Expr::Kind::Slice: {
    const auto &Slice = static_cast<const SliceExpr &>(Expression);
    OS << "(slice ";
    printExpr(*Slice.Base, OS);
    OS << ' ';
    printExpr(*Slice.Begin, OS);
    OS << ' ';
    printExpr(*Slice.End, OS);
    OS << ')';
    return;
  }
  case Expr::Kind::Cast: {
    const auto &Cast = static_cast<const CastExpr &>(Expression);
    OS << "(cast ";
    OS << Cast.Type.Name;
    if (Cast.Type.Width) {
      OS << '<';
      printExpr(*Cast.Type.Width, OS);
      OS << '>';
    }
    OS << ' ';
    printExpr(*Cast.Operand, OS);
    OS << ')';
    return;
  }
  }
}

void printType(const TypeRef &Type, llvm::raw_ostream &OS) {
  if (Type.isTensor()) {
    switch (Type.Storage) {
    case TypeRef::TensorStorage::Register:
      OS << "register ";
      break;
    case TypeRef::TensorStorage::Memory:
      OS << "memory ";
      break;
    case TypeRef::TensorStorage::Unspecified:
      OS << "unspecified ";
      break;
    }
    OS << "tensor<";
    if (Type.ElementType)
      printType(*Type.ElementType, OS);
    for (const auto &Dimension : Type.Shape) {
      OS << ", ";
      printExpr(*Dimension, OS);
    }
    OS << '>';
    return;
  }

  OS << Type.Name;
  if (!Type.Width)
    return;
  OS << '<';
  printExpr(*Type.Width, OS);
  OS << '>';
}

void printStmt(const Stmt &Statement, llvm::raw_ostream &OS, unsigned Depth) {
  indent(OS, Depth);
  switch (Statement.kind()) {
  case Stmt::Kind::Compound: {
    OS << "compound\n";
    for (const auto &Child :
         static_cast<const CompoundStmt &>(Statement).Statements)
      printStmt(*Child, OS, Depth + 1);
    return;
  }
  case Stmt::Kind::Expr: {
    OS << "expr ";
    printExpr(*static_cast<const ExprStmt &>(Statement).Expression, OS);
    OS << '\n';
    return;
  }
  case Stmt::Kind::If: {
    const auto &If = static_cast<const IfStmt &>(Statement);
    OS << "if ";
    printExpr(*If.Condition, OS);
    OS << '\n';
    printStmt(*If.Then, OS, Depth + 1);
    if (If.Else) {
      indent(OS, Depth);
      OS << "else\n";
      printStmt(*If.Else, OS, Depth + 1);
    }
    return;
  }
  case Stmt::Kind::Decl: {
    const auto &Decl = static_cast<const DeclStmt &>(Statement);
    OS << "decl ";
    printType(Decl.Type, OS);
    OS << ' ' << Decl.Name;
    if (Decl.Initializer) {
      OS << " = ";
      printExpr(*Decl.Initializer, OS);
    }
    OS << '\n';
    return;
  }
  case Stmt::Kind::Empty:
    OS << "empty\n";
    return;
  }
}

void printTokenSequence(const TokenSequence &Sequence, llvm::raw_ostream &OS) {
  for (size_t I = 0; I < Sequence.Tokens.size(); ++I) {
    if (I != 0)
      OS << ' ';
    OS << Sequence.Tokens[I].Text;
  }
}

} // namespace

void printAST(const InstructionSetDecl &Decl, llvm::raw_ostream &OS) {
  OS << "InstructionSet " << Decl.Name;
  if (!Decl.BaseName.empty())
    OS << " extends " << Decl.BaseName;
  OS << '\n';
  indent(OS, 1);
  OS << "instructions\n";
  for (const InstructionDecl &Instruction : Decl.Instructions) {
    indent(OS, 2);
    OS << "instruction " << Instruction.Name << '\n';
    for (const OperandDecl &Operand : Instruction.Operands) {
      indent(OS, 3);
      OS << "operand ";
      printType(Operand.Type, OS);
      OS << ' ' << Operand.Name << '\n';
    }
    if (Instruction.Encoding) {
      indent(OS, 3);
      OS << "encoding ";
      printTokenSequence(*Instruction.Encoding, OS);
      OS << '\n';
    }
    if (!Instruction.Assembly.empty()) {
      indent(OS, 3);
      OS << "assembly ";
      if (Instruction.Assembly.size() == 1) {
        OS << Instruction.Assembly.front();
      } else {
        OS << '{';
        for (size_t I = 0; I < Instruction.Assembly.size(); ++I) {
          if (I != 0)
            OS << ", ";
          OS << Instruction.Assembly[I];
        }
        OS << '}';
      }
      OS << '\n';
    }
    if (Instruction.Behavior) {
      indent(OS, 3);
      OS << "behavior\n";
      printStmt(*Instruction.Behavior, OS, 4);
    }
  }
}

} // namespace coredsl
