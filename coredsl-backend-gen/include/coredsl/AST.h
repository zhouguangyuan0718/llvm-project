#ifndef COREDSl_AST_H
#define COREDSl_AST_H

#include "coredsl/Lexer.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace coredsl {

class Expr {
public:
  enum class Kind {
    Identifier,
    Integer,
    String,
    Unary,
    Binary,
    Conditional,
    Index,
    Slice,
    Cast
  };

  explicit Expr(Kind K, SourceRange Range) : K(K), Range(std::move(Range)) {}
  virtual ~Expr() = default;

  Kind kind() const { return K; }
  const SourceRange &range() const { return Range; }

private:
  Kind K;
  SourceRange Range;
};

class IdentifierExpr final : public Expr {
public:
  IdentifierExpr(SourceRange Range, std::string Name)
      : Expr(Kind::Identifier, std::move(Range)), Name(std::move(Name)) {}
  std::string Name;
};

class LiteralExpr final : public Expr {
public:
  LiteralExpr(Kind K, SourceRange Range, std::string Value)
      : Expr(K, std::move(Range)), Value(std::move(Value)) {}
  std::string Value;
};

class UnaryExpr final : public Expr {
public:
  UnaryExpr(SourceRange Range, std::string Operator,
            std::unique_ptr<Expr> Operand, bool IsPostfix = false)
      : Expr(Kind::Unary, std::move(Range)), Operator(std::move(Operator)),
        Operand(std::move(Operand)), IsPostfix(IsPostfix) {}
  std::string Operator;
  std::unique_ptr<Expr> Operand;
  bool IsPostfix;
};

class BinaryExpr final : public Expr {
public:
  BinaryExpr(SourceRange Range, std::string Operator, std::unique_ptr<Expr> LHS,
             std::unique_ptr<Expr> RHS)
      : Expr(Kind::Binary, std::move(Range)), Operator(std::move(Operator)),
        LHS(std::move(LHS)), RHS(std::move(RHS)) {}
  std::string Operator;
  std::unique_ptr<Expr> LHS;
  std::unique_ptr<Expr> RHS;
};

class ConditionalExpr final : public Expr {
public:
  ConditionalExpr(SourceRange Range, std::unique_ptr<Expr> Condition,
                  std::unique_ptr<Expr> TrueExpr,
                  std::unique_ptr<Expr> FalseExpr)
      : Expr(Kind::Conditional, std::move(Range)),
        Condition(std::move(Condition)), TrueExpr(std::move(TrueExpr)),
        FalseExpr(std::move(FalseExpr)) {}
  std::unique_ptr<Expr> Condition;
  std::unique_ptr<Expr> TrueExpr;
  std::unique_ptr<Expr> FalseExpr;
};

class IndexExpr final : public Expr {
public:
  IndexExpr(SourceRange Range, std::unique_ptr<Expr> Base,
            std::unique_ptr<Expr> Index)
      : Expr(Kind::Index, std::move(Range)), Base(std::move(Base)),
        Index(std::move(Index)) {}
  std::unique_ptr<Expr> Base;
  std::unique_ptr<Expr> Index;
};

class SliceExpr final : public Expr {
public:
  SliceExpr(SourceRange Range, std::unique_ptr<Expr> Base,
            std::unique_ptr<Expr> Begin, std::unique_ptr<Expr> End)
      : Expr(Kind::Slice, std::move(Range)), Base(std::move(Base)),
        Begin(std::move(Begin)), End(std::move(End)) {}
  std::unique_ptr<Expr> Base;
  std::unique_ptr<Expr> Begin;
  std::unique_ptr<Expr> End;
};

struct TypeRef {
  SourceRange Range;
  std::string Name;
  std::unique_ptr<Expr> Width;
};

class CastExpr final : public Expr {
public:
  CastExpr(SourceRange Range, TypeRef Type, std::unique_ptr<Expr> Operand)
      : Expr(Kind::Cast, std::move(Range)), Type(std::move(Type)),
        Operand(std::move(Operand)) {}
  TypeRef Type;
  std::unique_ptr<Expr> Operand;
};

class Stmt {
public:
  enum class Kind { Compound, Expr, If, Decl, Empty };

  explicit Stmt(Kind K, SourceRange Range) : K(K), Range(std::move(Range)) {}
  virtual ~Stmt() = default;

  Kind kind() const { return K; }
  const SourceRange &range() const { return Range; }

private:
  Kind K;
  SourceRange Range;
};

class CompoundStmt final : public Stmt {
public:
  CompoundStmt(SourceRange Range, std::vector<std::unique_ptr<Stmt>> Statements)
      : Stmt(Kind::Compound, std::move(Range)),
        Statements(std::move(Statements)) {}
  std::vector<std::unique_ptr<Stmt>> Statements;
};

class ExprStmt final : public Stmt {
public:
  ExprStmt(SourceRange Range, std::unique_ptr<Expr> Expression)
      : Stmt(Kind::Expr, std::move(Range)), Expression(std::move(Expression)) {}
  std::unique_ptr<Expr> Expression;
};

class IfStmt final : public Stmt {
public:
  IfStmt(SourceRange Range, std::unique_ptr<Expr> Condition,
         std::unique_ptr<Stmt> Then, std::unique_ptr<Stmt> Else)
      : Stmt(Kind::If, std::move(Range)), Condition(std::move(Condition)),
        Then(std::move(Then)), Else(std::move(Else)) {}
  std::unique_ptr<Expr> Condition;
  std::unique_ptr<Stmt> Then;
  std::unique_ptr<Stmt> Else;
};

class DeclStmt final : public Stmt {
public:
  DeclStmt(SourceRange Range, TypeRef Type, std::string Name,
           std::unique_ptr<Expr> Initializer)
      : Stmt(Kind::Decl, std::move(Range)), Type(std::move(Type)),
        Name(std::move(Name)), Initializer(std::move(Initializer)) {}
  TypeRef Type;
  std::string Name;
  std::unique_ptr<Expr> Initializer;
};

class EmptyStmt final : public Stmt {
public:
  explicit EmptyStmt(SourceRange Range) : Stmt(Kind::Empty, std::move(Range)) {}
};

struct TokenSequence {
  SourceRange Range;
  std::vector<Token> Tokens;
};

struct OperandDecl {
  SourceRange Range;
  TypeRef Type;
  std::string Name;
};

struct InstructionDecl {
  SourceRange Range;
  std::string Name;
  std::vector<OperandDecl> Operands;
  std::unique_ptr<TokenSequence> Encoding;
  std::vector<std::string> Assembly;
  std::unique_ptr<Stmt> Behavior;
};

struct InstructionSetDecl {
  SourceRange Range;
  std::string Name;
  std::string BaseName;
  std::vector<InstructionDecl> Instructions;
};

} // namespace coredsl

#endif // COREDSl_AST_H
