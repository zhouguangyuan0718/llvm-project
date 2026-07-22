#ifndef COREDSl_LEXER_H
#define COREDSl_LEXER_H

#include "coredsl/Diagnostics.h"
#include "llvm/ADT/StringRef.h"

#include <string>
#include <vector>

namespace coredsl {

enum class TokenKind {
  Eof,
  Identifier,
  Integer,
  String,
  LBrace,
  RBrace,
  LParen,
  RParen,
  LBracket,
  RBracket,
  Semicolon,
  Colon,
  Comma,
  Dot,
  Question,
  Plus,
  Minus,
  Star,
  Slash,
  Percent,
  Amp,
  Pipe,
  Caret,
  Tilde,
  Bang,
  Equal,
  Less,
  Greater,
  PlusPlus,
  MinusMinus,
  PlusEqual,
  MinusEqual,
  StarEqual,
  SlashEqual,
  PercentEqual,
  AmpEqual,
  PipeEqual,
  CaretEqual,
  EqualEqual,
  BangEqual,
  LessEqual,
  GreaterEqual,
  LessLess,
  GreaterGreater,
  LessLessEqual,
  GreaterGreaterEqual,
  AmpAmp,
  PipePipe,
  Arrow,
  ColonColon,
};

struct Token {
  TokenKind Kind;
  std::string Text;
  SourceRange Range;
};

const char *tokenKindName(TokenKind Kind);

class Lexer {
public:
  Lexer(llvm::StringRef Input, DiagnosticEngine &Diags);

  std::vector<Token> lex();

private:
  bool atEnd() const;
  char peek(unsigned Offset = 0) const;
  char consume();
  SourceLocation location() const;
  void skipWhitespaceAndComments();
  Token lexIdentifier();
  Token lexInteger();
  Token lexString();
  Token lexPunctuation();
  Token makeToken(TokenKind Kind, std::string Text, SourceLocation Begin) const;

  llvm::StringRef Input;
  DiagnosticEngine &Diags;
  const char *Cursor;
  const char *End;
};

} // namespace coredsl

#endif // COREDSl_LEXER_H
