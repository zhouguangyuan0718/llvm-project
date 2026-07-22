#include "coredsl/ASTPrinter.h"

#include <ostream>
#include <string>

namespace coredsl {

namespace {

void indent(std::ostream &OS, unsigned Depth) {
  for (unsigned I = 0; I < Depth; ++I)
    OS << "  ";
}

void printExpr(const Expr &Expression, std::ostream &OS) {
  switch (Expression.kind()) {
  case Expr::Kind::Identifier:
    OS << static_cast<const IdentifierExpr &>(Expression).Name;
    return;
  case Expr::Kind::Integer:
  case Expr::Kind::String:
    OS << static_cast<const LiteralExpr &>(Expression).Value;
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
  case Expr::Kind::Call: {
    const auto &Call = static_cast<const CallExpr &>(Expression);
    OS << "(call ";
    printExpr(*Call.Callee, OS);
    for (const auto &Argument : Call.Arguments) {
      OS << ' ';
      printExpr(*Argument, OS);
    }
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
  case Expr::Kind::Member: {
    const auto &Member = static_cast<const MemberExpr &>(Expression);
    OS << "(member ";
    printExpr(*Member.Base, OS);
    OS << ' ' << Member.Member << ')';
    return;
  }
  }
}

void printStmt(const Stmt &Statement, std::ostream &OS, unsigned Depth) {
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
  case Stmt::Kind::Decl: {
    const auto &Decl = static_cast<const DeclStmt &>(Statement);
    OS << "decl " << Decl.TypeName << ' ' << Decl.Name;
    if (Decl.Initializer) {
      OS << " = ";
      printExpr(*Decl.Initializer, OS);
    }
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
  case Stmt::Kind::While: {
    const auto &While = static_cast<const WhileStmt &>(Statement);
    OS << "while ";
    printExpr(*While.Condition, OS);
    OS << '\n';
    printStmt(*While.Body, OS, Depth + 1);
    return;
  }
  case Stmt::Kind::For: {
    const auto &For = static_cast<const ForStmt &>(Statement);
    OS << "for\n";
    if (For.Init)
      printStmt(*For.Init, OS, Depth + 1);
    if (For.Condition) {
      indent(OS, Depth + 1);
      OS << "condition ";
      printExpr(*For.Condition, OS);
      OS << '\n';
    }
    if (For.Increment) {
      indent(OS, Depth + 1);
      OS << "increment ";
      printExpr(*For.Increment, OS);
      OS << '\n';
    }
    printStmt(*For.Body, OS, Depth + 1);
    return;
  }
  case Stmt::Kind::Empty:
    OS << "empty\n";
    return;
  }
}

void printTokenSequence(const TokenSequence &Sequence, std::ostream &OS) {
  for (size_t I = 0; I < Sequence.Tokens.size(); ++I) {
    if (I != 0)
      OS << ' ';
    OS << Sequence.Tokens[I].Text;
  }
}

} // namespace

void printAST(const InstructionSetDecl &Decl, std::ostream &OS) {
  OS << "InstructionSet " << Decl.Name;
  if (!Decl.BaseName.empty())
    OS << " extends " << Decl.BaseName;
  OS << '\n';
  indent(OS, 1);
  OS << "instructions\n";
  for (const InstructionDecl &Instruction : Decl.Instructions) {
    indent(OS, 2);
    OS << "instruction " << Instruction.Name << '\n';
    if (Instruction.Encoding) {
      indent(OS, 3);
      OS << "encoding ";
      printTokenSequence(*Instruction.Encoding, OS);
      OS << '\n';
    }
    if (Instruction.HasAssembly) {
      indent(OS, 3);
      OS << "assembly " << Instruction.Assembly << '\n';
    }
    if (Instruction.Behavior) {
      indent(OS, 3);
      OS << "behavior\n";
      printStmt(*Instruction.Behavior, OS, 4);
    }
    for (const auto &Other : Instruction.OtherMembers) {
      indent(OS, 3);
      OS << "member " << Other.first << ' ';
      printTokenSequence(Other.second, OS);
      OS << '\n';
    }
  }
}

} // namespace coredsl
