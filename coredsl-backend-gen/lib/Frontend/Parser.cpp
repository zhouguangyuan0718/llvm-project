#include "coredsl/Parser.h"

#include "coredsl/Lexer.h"

#include <utility>

namespace coredsl {

Parser::Parser(llvm::StringRef Input, DiagnosticEngine &Diags)
    : Diags(Diags), Tokens(Lexer(Input, Diags).lex()) {}

std::unique_ptr<InstructionSetDecl> Parser::parseInstructionSet() {
  if (!atIdentifier("InstructionSet")) {
    Diags.error(current().Range.Begin,
                "expected 'InstructionSet' at top level");
    return nullptr;
  }
  const SourceLocation Begin = consume().Range.Begin;

  std::string Name;
  if (!expectIdentifier(Name, "instruction-set name"))
    return nullptr;

  std::string BaseName;
  if (consumeIdentifierIf("extends") &&
      !expectIdentifier(BaseName, "base instruction-set name"))
    return nullptr;

  if (!expect(TokenKind::LBrace, "'{' after instruction-set declaration"))
    return nullptr;

  auto Result = std::make_unique<InstructionSetDecl>();
  Result->Name = std::move(Name);
  Result->BaseName = std::move(BaseName);

  bool SawInstructions = false;
  while (!at(TokenKind::RBrace) && !at(TokenKind::Eof)) {
    if (!consumeIdentifierIf("instructions")) {
      Diags.error(current().Range.Begin,
                  "expected 'instructions' block in instruction set");
      synchronizeStatement();
      continue;
    }
    if (SawInstructions)
      Diags.error(previous().Range.Begin, "duplicate 'instructions' block");
    SawInstructions = true;

    if (!expect(TokenKind::LBrace, "'{' after 'instructions'"))
      break;
    while (!at(TokenKind::RBrace) && !at(TokenKind::Eof)) {
      std::unique_ptr<InstructionDecl> Instruction = parseInstruction();
      if (Instruction)
        Result->Instructions.push_back(std::move(*Instruction));
      else
        synchronizeStatement();
    }
    expect(TokenKind::RBrace, "'}' after instructions block");
  }

  if (!SawInstructions)
    Diags.error(Begin, "instruction set has no 'instructions' block");
  if (!expect(TokenKind::RBrace, "'}' after instruction set"))
    return nullptr;
  Result->Range = rangeFrom(Begin);
  if (Diags.hasError())
    return nullptr;
  return Result;
}

const Token &Parser::current() const { return Tokens[Position]; }

const Token &Parser::previous() const {
  return Tokens[Position == 0 ? 0 : Position - 1];
}

bool Parser::at(TokenKind Kind) const { return current().Kind == Kind; }

bool Parser::atIdentifier(const char *Text) const {
  return at(TokenKind::Identifier) && current().Text == Text;
}

const Token &Parser::consume() {
  const Token &Result = current();
  if (!at(TokenKind::Eof))
    ++Position;
  return Result;
}

bool Parser::consumeIf(TokenKind Kind) {
  if (!at(Kind))
    return false;
  consume();
  return true;
}

bool Parser::consumeIdentifierIf(const char *Text) {
  if (!atIdentifier(Text))
    return false;
  consume();
  return true;
}

bool Parser::expect(TokenKind Kind, const char *What) {
  if (consumeIf(Kind))
    return true;
  Diags.error(current().Range.Begin, std::string("expected ") + What +
                                         ", got " +
                                         tokenKindName(current().Kind));
  return false;
}

bool Parser::expectIdentifier(std::string &Result, const char *What) {
  if (!at(TokenKind::Identifier)) {
    Diags.error(current().Range.Begin, std::string("expected ") + What +
                                           ", got " +
                                           tokenKindName(current().Kind));
    return false;
  }
  Result = consume().Text;
  return true;
}

void Parser::synchronizeStatement() {
  while (!at(TokenKind::Eof) && !at(TokenKind::RBrace)) {
    if (consumeIf(TokenKind::Semicolon))
      return;
    consume();
  }
}

std::unique_ptr<InstructionDecl> Parser::parseInstruction() {
  const SourceLocation Begin = current().Range.Begin;
  std::string Name;
  if (!expectIdentifier(Name, "instruction name"))
    return nullptr;
  if (!expect(TokenKind::LBrace, "'{' after instruction name"))
    return nullptr;

  auto Result = std::make_unique<InstructionDecl>();
  Result->Name = std::move(Name);
  while (!at(TokenKind::RBrace) && !at(TokenKind::Eof)) {
    std::string Member;
    if (!expectIdentifier(Member, "instruction member name")) {
      synchronizeStatement();
      continue;
    }
    consumeIf(TokenKind::Colon);
    consumeIf(TokenKind::Equal);

    if (Member == "encoding") {
      if (Result->Encoding)
        Diags.error(previous().Range.Begin, "duplicate 'encoding' member");
      Result->Encoding = std::make_unique<TokenSequence>(parseRawMemberValue());
      continue;
    }
    if (Member == "operands") {
      if (!Result->Operands.empty())
        Diags.error(previous().Range.Begin, "duplicate 'operands' member");
      Result->Operands = parseOperands();
      continue;
    }
    if (Member == "assembly") {
      if (!Result->Assembly.empty())
        Diags.error(previous().Range.Begin, "duplicate 'assembly' member");
      parseAssembly(*Result);
      continue;
    }
    if (Member == "behavior") {
      if (Result->Behavior)
        Diags.error(previous().Range.Begin, "duplicate 'behavior' member");
      Result->Behavior = parseStatement();
      continue;
    }

    Diags.error(previous().Range.Begin,
                "unsupported instruction member '" + Member + "'");
    parseRawMemberValue();
  }
  if (!expect(TokenKind::RBrace, "'}' after instruction"))
    return nullptr;
  Result->Range = rangeFrom(Begin);
  return Result;
}

std::vector<OperandDecl> Parser::parseOperands() {
  std::vector<OperandDecl> Result;
  if (!expect(TokenKind::LBrace, "'{' after 'operands'"))
    return Result;

  while (!at(TokenKind::RBrace) && !at(TokenKind::Eof)) {
    const SourceLocation Begin = current().Range.Begin;
    if (!looksLikeTypeRef()) {
      Diags.error(current().Range.Begin, "expected typed operand declaration");
      synchronizeStatement();
      continue;
    }
    TypeRef Type = parseTypeRef();
    std::string Name;
    if (!expectIdentifier(Name, "operand name")) {
      synchronizeStatement();
      continue;
    }
    if (!expect(TokenKind::Semicolon, "';' after operand declaration")) {
      synchronizeStatement();
      continue;
    }
    Result.push_back({rangeFrom(Begin), std::move(Type), std::move(Name)});
  }
  expect(TokenKind::RBrace, "'}' after operands block");
  consumeIf(TokenKind::Semicolon);
  return Result;
}

bool Parser::parseAssembly(InstructionDecl &Instruction) {
  if (at(TokenKind::String)) {
    Instruction.Assembly.push_back(consume().Text);
    return expect(TokenKind::Semicolon, "';' after assembly string");
  }
  if (!consumeIf(TokenKind::LBrace)) {
    Diags.error(current().Range.Begin,
                "expected string or '{' after 'assembly'");
    synchronizeStatement();
    return false;
  }
  if (!at(TokenKind::String)) {
    Diags.error(current().Range.Begin,
                "expected string literal in assembly declaration");
    synchronizeStatement();
    return false;
  }
  do {
    if (!at(TokenKind::String)) {
      Diags.error(current().Range.Begin,
                  "expected string literal in assembly declaration");
      return false;
    }
    Instruction.Assembly.push_back(consume().Text);
  } while (consumeIf(TokenKind::Comma));
  if (!expect(TokenKind::RBrace, "'}' after assembly declaration"))
    return false;
  return expect(TokenKind::Semicolon, "';' after assembly declaration");
}

TokenSequence Parser::parseRawMemberValue() {
  TokenSequence Result;
  Result.Range.Begin = current().Range.Begin;
  if (at(TokenKind::LBrace)) {
    unsigned Depth = 0;
    do {
      const Token Token = consume();
      if (Token.Kind == TokenKind::LBrace)
        ++Depth;
      else if (Token.Kind == TokenKind::RBrace)
        --Depth;
      Result.Tokens.push_back(Token);
    } while (Depth != 0 && !at(TokenKind::Eof));
    Result.Range.End = previous().Range.End;
    consumeIf(TokenKind::Semicolon);
    return Result;
  }

  unsigned ParenDepth = 0;
  unsigned BracketDepth = 0;
  while (!at(TokenKind::Eof)) {
    if (at(TokenKind::Semicolon) && ParenDepth == 0 && BracketDepth == 0)
      break;
    if (at(TokenKind::RBrace) && ParenDepth == 0 && BracketDepth == 0)
      break;
    const Token Token = consume();
    if (Token.Kind == TokenKind::LParen)
      ++ParenDepth;
    else if (Token.Kind == TokenKind::RParen && ParenDepth != 0)
      --ParenDepth;
    else if (Token.Kind == TokenKind::LBracket)
      ++BracketDepth;
    else if (Token.Kind == TokenKind::RBracket && BracketDepth != 0)
      --BracketDepth;
    Result.Tokens.push_back(Token);
  }
  if (Result.Tokens.empty())
    Diags.error(current().Range.Begin,
                "expected value after instruction member");
  Result.Range.End =
      Result.Tokens.empty() ? current().Range.End : previous().Range.End;
  expect(TokenKind::Semicolon, "';' after instruction member");
  return Result;
}

std::unique_ptr<Stmt> Parser::parseStatement() {
  if (at(TokenKind::LBrace))
    return parseCompoundStatement();
  if (atIdentifier("if"))
    return parseIfStatement();
  if (looksLikeTypeRef())
    return parseDeclarationStatement();
  if (at(TokenKind::Semicolon)) {
    const SourceLocation Begin = consume().Range.Begin;
    return std::make_unique<EmptyStmt>(rangeFrom(Begin));
  }

  const SourceLocation Begin = current().Range.Begin;
  std::unique_ptr<Expr> Expression = parseExpression();
  if (!Expression)
    return nullptr;
  if (!expect(TokenKind::Semicolon, "';' after expression"))
    return nullptr;
  return std::make_unique<ExprStmt>(rangeFrom(Begin), std::move(Expression));
}

std::unique_ptr<Stmt> Parser::parseDeclarationStatement() {
  const SourceLocation Begin = current().Range.Begin;
  TypeRef Type = parseTypeRef();
  std::string Name;
  if (!expectIdentifier(Name, "declaration name"))
    return nullptr;

  std::unique_ptr<Expr> Initializer;
  if (consumeIf(TokenKind::Equal)) {
    Initializer = parseExpression();
    if (!Initializer)
      return nullptr;
  }
  if (!expect(TokenKind::Semicolon, "';' after declaration"))
    return nullptr;
  return std::make_unique<DeclStmt>(rangeFrom(Begin), std::move(Type),
                                    std::move(Name), std::move(Initializer));
}

TypeRef Parser::parseTypeRef() {
  TypeRef Result;
  Result.Range.Begin = current().Range.Begin;
  expectIdentifier(Result.Name, "type name");
  if (consumeIf(TokenKind::Less)) {
    Result.Width = parsePrimaryExpression();
    if (!Result.Width || !expect(TokenKind::Greater, "'>' after type width"))
      return Result;
  }
  Result.Range.End = previous().Range.End;
  return Result;
}

std::unique_ptr<CompoundStmt> Parser::parseCompoundStatement() {
  const SourceLocation Begin = current().Range.Begin;
  if (!expect(TokenKind::LBrace, "'{'"))
    return nullptr;
  std::vector<std::unique_ptr<Stmt>> Statements;
  while (!at(TokenKind::RBrace) && !at(TokenKind::Eof)) {
    std::unique_ptr<Stmt> Statement = parseStatement();
    if (Statement)
      Statements.push_back(std::move(Statement));
    else
      synchronizeStatement();
  }
  if (!expect(TokenKind::RBrace, "'}' after compound statement"))
    return nullptr;
  return std::make_unique<CompoundStmt>(rangeFrom(Begin),
                                        std::move(Statements));
}

std::unique_ptr<Stmt> Parser::parseIfStatement() {
  const SourceLocation Begin = consume().Range.Begin;
  if (!expect(TokenKind::LParen, "'(' after 'if'"))
    return nullptr;
  std::unique_ptr<Expr> Condition = parseExpression();
  if (!Condition || !expect(TokenKind::RParen, "')' after if condition"))
    return nullptr;
  std::unique_ptr<Stmt> Then = parseStatement();
  std::unique_ptr<Stmt> Else;
  if (consumeIdentifierIf("else"))
    Else = parseStatement();
  return std::make_unique<IfStmt>(rangeFrom(Begin), std::move(Condition),
                                  std::move(Then), std::move(Else));
}

std::unique_ptr<Expr> Parser::parseExpression() {
  return parseAssignmentExpression();
}

std::unique_ptr<Expr> Parser::parseAssignmentExpression() {
  std::unique_ptr<Expr> LHS = parseConditionalExpression();
  if (!LHS || !isAssignmentOperator(current().Kind))
    return LHS;
  const std::string Operator = consume().Text;
  const SourceLocation Begin = LHS->range().Begin;
  std::unique_ptr<Expr> RHS = parseAssignmentExpression();
  if (!RHS)
    return nullptr;
  return std::make_unique<BinaryExpr>(rangeFrom(Begin), Operator,
                                      std::move(LHS), std::move(RHS));
}

std::unique_ptr<Expr> Parser::parseConditionalExpression() {
  std::unique_ptr<Expr> Condition = parseBinaryExpression(1);
  if (!Condition || !consumeIf(TokenKind::Question))
    return Condition;
  const SourceLocation Begin = Condition->range().Begin;
  std::unique_ptr<Expr> TrueExpr = parseExpression();
  if (!TrueExpr || !expect(TokenKind::Colon, "':' in conditional expression"))
    return nullptr;
  std::unique_ptr<Expr> FalseExpr = parseAssignmentExpression();
  if (!FalseExpr)
    return nullptr;
  return std::make_unique<ConditionalExpr>(
      rangeFrom(Begin), std::move(Condition), std::move(TrueExpr),
      std::move(FalseExpr));
}

std::unique_ptr<Expr> Parser::parseBinaryExpression(unsigned MinPrecedence) {
  std::unique_ptr<Expr> LHS = parseUnaryExpression();
  if (!LHS)
    return nullptr;
  while (true) {
    const int Precedence = binaryPrecedence(current().Kind);
    if (Precedence < static_cast<int>(MinPrecedence))
      break;
    const std::string Operator = consume().Text;
    const SourceLocation Begin = LHS->range().Begin;
    std::unique_ptr<Expr> RHS = parseBinaryExpression(Precedence + 1);
    if (!RHS)
      return nullptr;
    LHS = std::make_unique<BinaryExpr>(rangeFrom(Begin), std::move(Operator),
                                       std::move(LHS), std::move(RHS));
  }
  return LHS;
}

std::unique_ptr<Expr> Parser::parseUnaryExpression() {
  if (looksLikeCast()) {
    const SourceLocation Begin = consume().Range.Begin;
    TypeRef Type = parseTypeRef();
    if (!expect(TokenKind::RParen, "')' after cast type"))
      return nullptr;
    std::unique_ptr<Expr> Operand = parseUnaryExpression();
    if (!Operand)
      return nullptr;
    return std::make_unique<CastExpr>(rangeFrom(Begin), std::move(Type),
                                      std::move(Operand));
  }
  switch (current().Kind) {
  case TokenKind::Plus:
  case TokenKind::Minus:
  case TokenKind::Bang:
  case TokenKind::Tilde:
  case TokenKind::PlusPlus:
  case TokenKind::MinusMinus: {
    const Token Operator = consume();
    std::unique_ptr<Expr> Operand = parseUnaryExpression();
    if (!Operand)
      return nullptr;
    return std::make_unique<UnaryExpr>(rangeFrom(Operator.Range.Begin),
                                       Operator.Text, std::move(Operand));
  }
  default:
    return parsePostfixExpression();
  }
}

std::unique_ptr<Expr> Parser::parsePostfixExpression() {
  std::unique_ptr<Expr> Result = parsePrimaryExpression();
  if (!Result)
    return nullptr;
  while (true) {
    const SourceLocation Begin = Result->range().Begin;
    if (consumeIf(TokenKind::LBracket)) {
      std::unique_ptr<Expr> Index = parseExpression();
      if (!Index)
        return nullptr;
      if (consumeIf(TokenKind::Colon)) {
        std::unique_ptr<Expr> End = parseExpression();
        if (!End || !expect(TokenKind::RBracket, "']' after slice expression"))
          return nullptr;
        Result =
            std::make_unique<SliceExpr>(rangeFrom(Begin), std::move(Result),
                                        std::move(Index), std::move(End));
        continue;
      }
      if (!expect(TokenKind::RBracket, "']' after index expression"))
        return nullptr;
      Result = std::make_unique<IndexExpr>(rangeFrom(Begin), std::move(Result),
                                           std::move(Index));
      continue;
    }
    if (at(TokenKind::PlusPlus) || at(TokenKind::MinusMinus)) {
      const std::string Operator = consume().Text;
      Result = std::make_unique<UnaryExpr>(rangeFrom(Begin), Operator,
                                           std::move(Result), true);
      continue;
    }
    return Result;
  }
}

bool Parser::looksLikeCast() const {
  if (!at(TokenKind::LParen) || Position + 2 >= Tokens.size() ||
      Tokens[Position + 1].Kind != TokenKind::Identifier)
    return false;
  if (Tokens[Position + 2].Kind == TokenKind::RParen)
    return Tokens[Position + 1].Text == "signed" ||
           Tokens[Position + 1].Text == "unsigned";
  if (Tokens[Position + 2].Kind != TokenKind::Less)
    return false;

  size_t Cursor = Position + 3;
  unsigned Depth = 1;
  while (Cursor < Tokens.size()) {
    if (Tokens[Cursor].Kind == TokenKind::Less)
      ++Depth;
    else if (Tokens[Cursor].Kind == TokenKind::Greater && --Depth == 0)
      return Cursor + 1 < Tokens.size() &&
             Tokens[Cursor + 1].Kind == TokenKind::RParen;
    ++Cursor;
  }
  return false;
}

bool Parser::looksLikeTypeRef() const {
  if (!at(TokenKind::Identifier) || Position + 1 >= Tokens.size())
    return false;
  if (Tokens[Position + 1].Kind == TokenKind::Less)
    return true;
  return (current().Text == "signed" || current().Text == "unsigned") &&
         Tokens[Position + 1].Kind == TokenKind::Identifier;
}

std::unique_ptr<Expr> Parser::parsePrimaryExpression() {
  const Token Token = current();
  if (consumeIf(TokenKind::Identifier))
    return std::make_unique<IdentifierExpr>(Token.Range, Token.Text);
  if (consumeIf(TokenKind::Integer))
    return std::make_unique<LiteralExpr>(Expr::Kind::Integer, Token.Range,
                                         Token.Text);
  if (consumeIf(TokenKind::LParen)) {
    std::unique_ptr<Expr> Expression = parseExpression();
    if (!Expression || !expect(TokenKind::RParen, "')' after expression"))
      return nullptr;
    return Expression;
  }
  Diags.error(current().Range.Begin, "expected expression");
  return nullptr;
}

int Parser::binaryPrecedence(TokenKind Kind) const {
  switch (Kind) {
  case TokenKind::PipePipe:
    return 1;
  case TokenKind::AmpAmp:
    return 2;
  case TokenKind::Pipe:
    return 3;
  case TokenKind::Caret:
    return 4;
  case TokenKind::Amp:
    return 5;
  case TokenKind::EqualEqual:
  case TokenKind::BangEqual:
    return 6;
  case TokenKind::Less:
  case TokenKind::LessEqual:
  case TokenKind::Greater:
  case TokenKind::GreaterEqual:
    return 7;
  case TokenKind::LessLess:
  case TokenKind::GreaterGreater:
    return 8;
  case TokenKind::Plus:
  case TokenKind::Minus:
    return 9;
  case TokenKind::Star:
  case TokenKind::Slash:
  case TokenKind::Percent:
    return 10;
  default:
    return -1;
  }
}

bool Parser::isAssignmentOperator(TokenKind Kind) const {
  switch (Kind) {
  case TokenKind::Equal:
  case TokenKind::PlusEqual:
  case TokenKind::MinusEqual:
  case TokenKind::StarEqual:
  case TokenKind::SlashEqual:
  case TokenKind::PercentEqual:
  case TokenKind::AmpEqual:
  case TokenKind::PipeEqual:
  case TokenKind::CaretEqual:
  case TokenKind::LessLessEqual:
  case TokenKind::GreaterGreaterEqual:
    return true;
  default:
    return false;
  }
}

SourceRange Parser::rangeFrom(const SourceLocation &Begin) const {
  return {Begin, previous().Range.End};
}

} // namespace coredsl
