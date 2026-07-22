#include "coredsl/Lexer.h"

#include <cctype>
#include <utility>

namespace coredsl {

namespace {

bool isIdentifierStart(char C) {
  return std::isalpha(static_cast<unsigned char>(C)) || C == '_';
}

bool isIdentifierContinue(char C) {
  return std::isalnum(static_cast<unsigned char>(C)) || C == '_';
}

} // namespace

const char *tokenKindName(TokenKind Kind) {
  switch (Kind) {
  case TokenKind::Eof:
    return "end of file";
  case TokenKind::Identifier:
    return "identifier";
  case TokenKind::Integer:
    return "integer";
  case TokenKind::String:
    return "string";
  case TokenKind::LBrace:
    return "{";
  case TokenKind::RBrace:
    return "}";
  case TokenKind::LParen:
    return "(";
  case TokenKind::RParen:
    return ")";
  case TokenKind::LBracket:
    return "[";
  case TokenKind::RBracket:
    return "]";
  case TokenKind::Semicolon:
    return ";";
  case TokenKind::Colon:
    return ":";
  case TokenKind::Comma:
    return ",";
  case TokenKind::Dot:
    return ".";
  case TokenKind::Question:
    return "?";
  case TokenKind::Plus:
    return "+";
  case TokenKind::Minus:
    return "-";
  case TokenKind::Star:
    return "*";
  case TokenKind::Slash:
    return "/";
  case TokenKind::Percent:
    return "%";
  case TokenKind::Amp:
    return "&";
  case TokenKind::Pipe:
    return "|";
  case TokenKind::Caret:
    return "^";
  case TokenKind::Tilde:
    return "~";
  case TokenKind::Bang:
    return "!";
  case TokenKind::Equal:
    return "=";
  case TokenKind::Less:
    return "<";
  case TokenKind::Greater:
    return ">";
  case TokenKind::PlusPlus:
    return "++";
  case TokenKind::MinusMinus:
    return "--";
  case TokenKind::PlusEqual:
    return "+=";
  case TokenKind::MinusEqual:
    return "-=";
  case TokenKind::StarEqual:
    return "*=";
  case TokenKind::SlashEqual:
    return "/=";
  case TokenKind::PercentEqual:
    return "%=";
  case TokenKind::AmpEqual:
    return "&=";
  case TokenKind::PipeEqual:
    return "|=";
  case TokenKind::CaretEqual:
    return "^=";
  case TokenKind::EqualEqual:
    return "==";
  case TokenKind::BangEqual:
    return "!=";
  case TokenKind::LessEqual:
    return "<=";
  case TokenKind::GreaterEqual:
    return ">=";
  case TokenKind::LessLess:
    return "<<";
  case TokenKind::GreaterGreater:
    return ">>";
  case TokenKind::LessLessEqual:
    return "<<=";
  case TokenKind::GreaterGreaterEqual:
    return ">>=";
  case TokenKind::AmpAmp:
    return "&&";
  case TokenKind::PipePipe:
    return "||";
  case TokenKind::Arrow:
    return "->";
  case TokenKind::ColonColon:
    return "::";
  }
  return "unknown token";
}

Lexer::Lexer(std::string FileName, std::string Input, DiagnosticEngine &Diags)
    : FileName(std::move(FileName)), Input(std::move(Input)), Diags(Diags) {}

std::vector<Token> Lexer::lex() {
  std::vector<Token> Tokens;
  while (!atEnd()) {
    skipWhitespaceAndComments();
    if (atEnd())
      break;

    const char C = peek();
    if (isIdentifierStart(C))
      Tokens.push_back(lexIdentifier());
    else if (std::isdigit(static_cast<unsigned char>(C)))
      Tokens.push_back(lexInteger());
    else if (C == '"')
      Tokens.push_back(lexString());
    else
      Tokens.push_back(lexPunctuation());
  }

  const SourceLocation End = location();
  Tokens.push_back({TokenKind::Eof, "", {End, End}});
  return Tokens;
}

bool Lexer::atEnd() const { return Offset == Input.size(); }

char Lexer::peek(unsigned OffsetFromCurrent) const {
  const size_t Index = Offset + OffsetFromCurrent;
  return Index < Input.size() ? Input[Index] : '\0';
}

char Lexer::consume() {
  const char C = Input[Offset++];
  if (C == '\n') {
    ++Line;
    Column = 1;
  } else {
    ++Column;
  }
  return C;
}

SourceLocation Lexer::location() const { return {FileName, Line, Column}; }

void Lexer::skipWhitespaceAndComments() {
  while (!atEnd()) {
    if (std::isspace(static_cast<unsigned char>(peek()))) {
      consume();
      continue;
    }
    if (peek() == '/' && peek(1) == '/') {
      while (!atEnd() && peek() != '\n')
        consume();
      continue;
    }
    if (peek() == '/' && peek(1) == '*') {
      const SourceLocation Begin = location();
      consume();
      consume();
      while (!atEnd() && !(peek() == '*' && peek(1) == '/'))
        consume();
      if (atEnd()) {
        Diags.error(Begin, "unterminated block comment");
        return;
      }
      consume();
      consume();
      continue;
    }
    break;
  }
}

Token Lexer::lexIdentifier() {
  const SourceLocation Begin = location();
  std::string Text;
  do {
    Text.push_back(consume());
  } while (isIdentifierContinue(peek()));
  return makeToken(TokenKind::Identifier, std::move(Text), Begin);
}

Token Lexer::lexInteger() {
  const SourceLocation Begin = location();
  std::string Text;
  if (peek() == '0' &&
      (peek(1) == 'x' || peek(1) == 'X' || peek(1) == 'b' || peek(1) == 'B')) {
    Text.push_back(consume());
    Text.push_back(consume());
  }
  while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')
    Text.push_back(consume());
  return makeToken(TokenKind::Integer, std::move(Text), Begin);
}

Token Lexer::lexString() {
  const SourceLocation Begin = location();
  std::string Text;
  Text.push_back(consume());
  bool Terminated = false;
  while (!atEnd()) {
    const char C = consume();
    Text.push_back(C);
    if (C == '\\' && !atEnd()) {
      Text.push_back(consume());
      continue;
    }
    if (C == '"') {
      Terminated = true;
      break;
    }
    if (C == '\n')
      break;
  }
  if (!Terminated)
    Diags.error(Begin, "unterminated string literal");
  return makeToken(TokenKind::String, std::move(Text), Begin);
}

Token Lexer::lexPunctuation() {
  const SourceLocation Begin = location();
  const char C = consume();
  const char Next = peek();
  auto two = [&](char Expected, TokenKind One, TokenKind Two) {
    if (Next == Expected) {
      std::string Text;
      Text.push_back(C);
      Text.push_back(consume());
      return makeToken(Two, std::move(Text), Begin);
    }
    return makeToken(One, std::string(1, C), Begin);
  };

  switch (C) {
  case '{':
    return makeToken(TokenKind::LBrace, "{", Begin);
  case '}':
    return makeToken(TokenKind::RBrace, "}", Begin);
  case '(':
    return makeToken(TokenKind::LParen, "(", Begin);
  case ')':
    return makeToken(TokenKind::RParen, ")", Begin);
  case '[':
    return makeToken(TokenKind::LBracket, "[", Begin);
  case ']':
    return makeToken(TokenKind::RBracket, "]", Begin);
  case ';':
    return makeToken(TokenKind::Semicolon, ";", Begin);
  case ',':
    return makeToken(TokenKind::Comma, ",", Begin);
  case '.':
    return makeToken(TokenKind::Dot, ".", Begin);
  case '?':
    return makeToken(TokenKind::Question, "?", Begin);
  case '~':
    return makeToken(TokenKind::Tilde, "~", Begin);
  case '+':
    if (Next == '=') {
      consume();
      return makeToken(TokenKind::PlusEqual, "+=", Begin);
    }
    return two('+', TokenKind::Plus, TokenKind::PlusPlus);
  case '-':
    if (Next == '>') {
      consume();
      return makeToken(TokenKind::Arrow, "->", Begin);
    }
    if (Next == '=') {
      consume();
      return makeToken(TokenKind::MinusEqual, "-=", Begin);
    }
    return two('-', TokenKind::Minus, TokenKind::MinusMinus);
  case '*':
    return two('=', TokenKind::Star, TokenKind::StarEqual);
  case '/':
    return two('=', TokenKind::Slash, TokenKind::SlashEqual);
  case '%':
    return two('=', TokenKind::Percent, TokenKind::PercentEqual);
  case '&':
    if (Next == '&') {
      consume();
      return makeToken(TokenKind::AmpAmp, "&&", Begin);
    }
    return two('=', TokenKind::Amp, TokenKind::AmpEqual);
  case '|':
    if (Next == '|') {
      consume();
      return makeToken(TokenKind::PipePipe, "||", Begin);
    }
    return two('=', TokenKind::Pipe, TokenKind::PipeEqual);
  case '^':
    return two('=', TokenKind::Caret, TokenKind::CaretEqual);
  case ':':
    return two(':', TokenKind::Colon, TokenKind::ColonColon);
  case '=':
    return two('=', TokenKind::Equal, TokenKind::EqualEqual);
  case '!':
    return two('=', TokenKind::Bang, TokenKind::BangEqual);
  case '<':
    if (Next == '<') {
      consume();
      if (peek() == '=') {
        consume();
        return makeToken(TokenKind::LessLessEqual, "<<=", Begin);
      }
      return makeToken(TokenKind::LessLess, "<<", Begin);
    }
    return two('=', TokenKind::Less, TokenKind::LessEqual);
  case '>':
    if (Next == '>') {
      consume();
      if (peek() == '=') {
        consume();
        return makeToken(TokenKind::GreaterGreaterEqual, ">>=", Begin);
      }
      return makeToken(TokenKind::GreaterGreater, ">>", Begin);
    }
    return two('=', TokenKind::Greater, TokenKind::GreaterEqual);
  default:
    Diags.error(Begin, std::string("unexpected character '") + C + "'");
    return makeToken(TokenKind::Identifier, std::string(1, C), Begin);
  }
}

Token Lexer::makeToken(TokenKind Kind, std::string Text,
                       SourceLocation Begin) const {
  return {Kind, std::move(Text), {std::move(Begin), location()}};
}

} // namespace coredsl
