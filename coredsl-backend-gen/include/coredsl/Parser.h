#ifndef COREDSl_PARSER_H
#define COREDSl_PARSER_H

#include "coredsl/AST.h"
#include "llvm/ADT/StringRef.h"

#include <memory>
#include <string>
#include <vector>

namespace coredsl {

class Parser {
public:
  Parser(llvm::StringRef Input, DiagnosticEngine &Diags);

  std::unique_ptr<InstructionSetDecl> parseInstructionSet();

private:
  const Token &current() const;
  const Token &previous() const;
  bool at(TokenKind Kind) const;
  bool atIdentifier(const char *Text) const;
  const Token &consume();
  bool consumeIf(TokenKind Kind);
  bool consumeIdentifierIf(const char *Text);
  bool expect(TokenKind Kind, const char *What);
  bool expectIdentifier(std::string &Result, const char *What);
  void synchronizeStatement();

  std::unique_ptr<InstructionDecl> parseInstruction();
  std::vector<OperandDecl> parseOperands();
  bool parseAssembly(InstructionDecl &Instruction);
  TokenSequence parseRawMemberValue();
  std::unique_ptr<Stmt> parseStatement();
  std::unique_ptr<CompoundStmt> parseCompoundStatement();
  std::unique_ptr<Stmt> parseIfStatement();
  std::unique_ptr<Stmt> parseDeclarationStatement();

  TypeRef parseTypeRef();
  TypeRef parseScalarTypeRef();
  TypeRef parseTensorTypeRef(TypeRef::TensorStorage Storage,
                             SourceLocation Begin);
  std::unique_ptr<Expr> parseTypeParameter(const char *What,
                                           bool AllowDynamic = false);
  std::unique_ptr<Expr> parseExpression();
  std::unique_ptr<Expr> parseAssignmentExpression();
  std::unique_ptr<Expr> parseConditionalExpression();
  std::unique_ptr<Expr> parseBinaryExpression(unsigned MinPrecedence);
  std::unique_ptr<Expr> parseUnaryExpression();
  std::unique_ptr<Expr> parsePostfixExpression();
  std::unique_ptr<Expr> parsePrimaryExpression();
  bool looksLikeCast() const;
  bool looksLikeTypeRef() const;
  int binaryPrecedence(TokenKind Kind) const;
  bool isAssignmentOperator(TokenKind Kind) const;
  SourceRange rangeFrom(const SourceLocation &Begin) const;

  DiagnosticEngine &Diags;
  std::vector<Token> Tokens;
  size_t Position = 0;
};

} // namespace coredsl

#endif // COREDSl_PARSER_H
