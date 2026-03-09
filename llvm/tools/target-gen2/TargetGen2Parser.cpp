//===- TargetGen2Parser.cpp - CoreDSL parser -------------------*- C++ -*-===//

#include "TargetGen2Parser.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"
#include <cctype>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;
using namespace llvm::targetgen2;

namespace {

struct SourceLocation {
  size_t Offset = 0;
  unsigned Line = 1;
  unsigned Column = 1;
};

enum class TokenKind {
  Eof,
  Identifier,
  Integer,
  String,
  Other,
  LBrace,
  RBrace,
  LParen,
  RParen,
  LBracket,
  RBracket,
  DoubleLBracket,
  DoubleRBracket,
  Semicolon,
  Comma,
  Colon,
  DoubleColon,
  Equal,
  KwImport,
  KwInstructionSet,
  KwCombines,
  KwExtends,
  KwCore,
  KwProvides,
  KwArchitecturalState,
  KwFunctions,
  KwInstructions,
  KwAlways,
  KwEncoding,
  KwAssembly,
  KwBehavior,
};

struct Token {
  TokenKind Kind = TokenKind::Eof;
  StringRef Lexeme;
  SourceLocation Loc;
  size_t EndOffset = 0;
};

struct Lexer {
  explicit Lexer(StringRef Buffer) : Buffer(Buffer) { AtEnd = Buffer.empty(); }

  Token next();

private:
  StringRef Buffer;
  size_t Index = 0;
  SourceLocation Loc;
  bool AtEnd = false;

  char peek() const { return Buffer[Index]; }
  char peekAhead(size_t N) const {
    size_t Pos = Index + N;
    return Pos < Buffer.size() ? Buffer[Pos] : '\0';
  }
  void advance();
  void skipTrivia();
  Token lexIdentifierOrKeyword();
  Token lexString();
  Token lexInteger();
};

void Lexer::advance() {
  if (Index >= Buffer.size()) {
    AtEnd = true;
    return;
  }
  if (Buffer[Index] == '\n') {
    ++Loc.Line;
    Loc.Column = 1;
  } else {
    ++Loc.Column;
  }
  ++Index;
  Loc.Offset = Index;
  AtEnd = Index >= Buffer.size();
}

void Lexer::skipTrivia() {
  while (!AtEnd) {
    if (isspace(static_cast<unsigned char>(peek()))) {
      advance();
      continue;
    }
    if (peek() == '/' && peekAhead(1) == '/') {
      advance();
      advance();
      while (!AtEnd && peek() != '\n')
        advance();
      continue;
    }
    if (peek() == '/' && peekAhead(1) == '*') {
      advance();
      advance();
      while (!AtEnd) {
        if (peek() == '*' && peekAhead(1) == '/') {
          advance();
          advance();
          break;
        }
        advance();
      }
      continue;
    }
    break;
  }
}

Token Lexer::lexIdentifierOrKeyword() {
  SourceLocation Start = Loc;
  size_t StartIdx = Index;
  while (!AtEnd && (isalnum(static_cast<unsigned char>(peek())) || peek() == '_'))
    advance();
  StringRef Lexeme = Buffer.slice(StartIdx, Index);
  TokenKind Kind = TokenKind::Identifier;
  if (Lexeme == "import") Kind = TokenKind::KwImport;
  else if (Lexeme == "InstructionSet") Kind = TokenKind::KwInstructionSet;
  else if (Lexeme == "combines") Kind = TokenKind::KwCombines;
  else if (Lexeme == "extends") Kind = TokenKind::KwExtends;
  else if (Lexeme == "Core") Kind = TokenKind::KwCore;
  else if (Lexeme == "provides") Kind = TokenKind::KwProvides;
  else if (Lexeme == "architectural_state") Kind = TokenKind::KwArchitecturalState;
  else if (Lexeme == "functions") Kind = TokenKind::KwFunctions;
  else if (Lexeme == "instructions") Kind = TokenKind::KwInstructions;
  else if (Lexeme == "always") Kind = TokenKind::KwAlways;
  else if (Lexeme == "encoding") Kind = TokenKind::KwEncoding;
  else if (Lexeme == "assembly") Kind = TokenKind::KwAssembly;
  else if (Lexeme == "behavior") Kind = TokenKind::KwBehavior;
  return Token{Kind, Lexeme, Start, Index};
}

Token Lexer::lexString() {
  SourceLocation Start = Loc;
  size_t StartIdx = Index;
  advance();
  while (!AtEnd) {
    if (peek() == '\\') {
      advance();
      if (!AtEnd)
        advance();
      continue;
    }
    if (peek() == '"') {
      advance();
      break;
    }
    advance();
  }
  return Token{TokenKind::String, Buffer.slice(StartIdx, Index), Start, Index};
}

Token Lexer::lexInteger() {
  SourceLocation Start = Loc;
  size_t StartIdx = Index;
  while (!AtEnd &&
         (isalnum(static_cast<unsigned char>(peek())) || peek() == '\'' || peek() == '_'))
    advance();
  return Token{TokenKind::Integer, Buffer.slice(StartIdx, Index), Start, Index};
}

Token Lexer::next() {
  skipTrivia();
  if (AtEnd)
    return Token{TokenKind::Eof, "", Loc, Loc.Offset};

  SourceLocation Start = Loc;
  size_t StartIdx = Index;
  char C = peek();
  if (isalpha(static_cast<unsigned char>(C)) || C == '_')
    return lexIdentifierOrKeyword();
  if (isdigit(static_cast<unsigned char>(C)))
    return lexInteger();
  if (C == '"')
    return lexString();

  if (C == ':' && peekAhead(1) == ':') {
    advance();
    advance();
    return Token{TokenKind::DoubleColon, Buffer.slice(StartIdx, Index), Start, Index};
  }
  if (C == '[' && peekAhead(1) == '[') {
    advance();
    advance();
    return Token{TokenKind::DoubleLBracket, Buffer.slice(StartIdx, Index), Start, Index};
  }
  if (C == ']' && peekAhead(1) == ']') {
    advance();
    advance();
    return Token{TokenKind::DoubleRBracket, Buffer.slice(StartIdx, Index), Start, Index};
  }

  advance();
  switch (C) {
  case '{': return Token{TokenKind::LBrace, "{", Start, Index};
  case '}': return Token{TokenKind::RBrace, "}", Start, Index};
  case '(': return Token{TokenKind::LParen, "(", Start, Index};
  case ')': return Token{TokenKind::RParen, ")", Start, Index};
  case '[': return Token{TokenKind::LBracket, "[", Start, Index};
  case ']': return Token{TokenKind::RBracket, "]", Start, Index};
  case ';': return Token{TokenKind::Semicolon, ";", Start, Index};
  case ',': return Token{TokenKind::Comma, ",", Start, Index};
  case ':': return Token{TokenKind::Colon, ":", Start, Index};
  case '=': return Token{TokenKind::Equal, "=", Start, Index};
  default: return Token{TokenKind::Other, Buffer.slice(StartIdx, Index), Start, Index};
  }
}

struct ExprTok {
  enum Kind { End, Ident, Number, String, LParen, RParen, LBracket, RBracket,
              Comma, Dot, Colon, Question, Op } K = End;
  StringRef Text;
  size_t B = 0;
  size_t E = 0;
};

class ExprTokenizer {
public:
  explicit ExprTokenizer(StringRef S) : S(S) {}
  std::vector<ExprTok> tokenize() {
    std::vector<ExprTok> T;
    while (I < S.size()) {
      char C = S[I];
      if (isspace(static_cast<unsigned char>(C))) {
        ++I;
        continue;
      }
      size_t B = I;
      if (isalpha(static_cast<unsigned char>(C)) || C == '_') {
        ++I;
        while (I < S.size() &&
               (isalnum(static_cast<unsigned char>(S[I])) || S[I] == '_'))
          ++I;
        T.push_back({ExprTok::Ident, S.slice(B, I), B, I});
        continue;
      }
      if (isdigit(static_cast<unsigned char>(C))) {
        ++I;
        while (I < S.size() &&
               (isalnum(static_cast<unsigned char>(S[I])) || S[I] == '\'' || S[I] == '_'))
          ++I;
        T.push_back({ExprTok::Number, S.slice(B, I), B, I});
        continue;
      }
      if (C == '"') {
        ++I;
        while (I < S.size()) {
          if (S[I] == '\\') {
            I += 2;
            continue;
          }
          if (S[I] == '"') {
            ++I;
            break;
          }
          ++I;
        }
        T.push_back({ExprTok::String, S.slice(B, I), B, I});
        continue;
      }
      if (C == '(') { ++I; T.push_back({ExprTok::LParen, S.slice(B, I), B, I}); continue; }
      if (C == ')') { ++I; T.push_back({ExprTok::RParen, S.slice(B, I), B, I}); continue; }
      if (C == '[') { ++I; T.push_back({ExprTok::LBracket, S.slice(B, I), B, I}); continue; }
      if (C == ']') { ++I; T.push_back({ExprTok::RBracket, S.slice(B, I), B, I}); continue; }
      if (C == ',') { ++I; T.push_back({ExprTok::Comma, S.slice(B, I), B, I}); continue; }
      if (C == '.') { ++I; T.push_back({ExprTok::Dot, S.slice(B, I), B, I}); continue; }
      if (C == ':') { ++I; T.push_back({ExprTok::Colon, S.slice(B, I), B, I}); continue; }
      if (C == '?') { ++I; T.push_back({ExprTok::Question, S.slice(B, I), B, I}); continue; }

      StringRef Two = I + 1 < S.size() ? S.slice(I, I + 2) : StringRef();
      StringRef Three = I + 2 < S.size() ? S.slice(I, I + 3) : StringRef();
      if (Three == "<<=" || Three == ">>=") {
        I += 3;
        T.push_back({ExprTok::Op, Three, B, I});
        continue;
      }
      if (Two == "==" || Two == "!=" || Two == "<=" || Two == ">=" ||
          Two == "&&" || Two == "||" || Two == "<<" || Two == ">>" ||
          Two == "++" || Two == "--" || Two == "+=" || Two == "-=" ||
          Two == "*=" || Two == "/=" || Two == "%=" || Two == "&=" ||
          Two == "|=" || Two == "^=" || Two == "::") {
        I += 2;
        T.push_back({ExprTok::Op, Two, B, I});
        continue;
      }
      ++I;
      T.push_back({ExprTok::Op, S.slice(B, I), B, I});
    }
    T.push_back({ExprTok::End, "", S.size(), S.size()});
    return T;
  }

private:
  StringRef S;
  size_t I = 0;
};

class ExprParser {
public:
  explicit ExprParser(StringRef Text) : Source(Text), Tokens(ExprTokenizer(Text).tokenize()) {}
  Expr parse() { return parseAssignment(); }

private:
  StringRef Source;
  std::vector<ExprTok> Tokens;
  size_t I = 0;

  const ExprTok &cur() const { return Tokens[I]; }
  const ExprTok &next() const { return Tokens[I + 1]; }
  void adv() { if (I < Tokens.size() - 1) ++I; }
  StringRef text(size_t B, size_t E) const { return Source.slice(B, E).trim(); }

  Expr makeNode(ExprKind Kind, size_t B, size_t E, std::vector<Expr> Children = {},
                StringRef Op = "", StringRef Value = "") {
    Expr R;
    R.Kind = Kind;
    R.Text = text(B, E).str();
    R.Op = Op.str();
    R.Value = Value.str();
    R.Children = std::move(Children);
    return R;
  }

  int precedence(StringRef Op) const {
    if (Op == "||") return 1;
    if (Op == "&&") return 2;
    if (Op == "|" ) return 3;
    if (Op == "^" ) return 4;
    if (Op == "&" ) return 5;
    if (Op == "==" || Op == "!=") return 6;
    if (Op == "<" || Op == ">" || Op == "<=" || Op == ">=") return 7;
    if (Op == "<<" || Op == ">>") return 8;
    if (Op == "+" || Op == "-") return 9;
    if (Op == "*" || Op == "/" || Op == "%") return 10;
    return -1;
  }

  bool isAssignOp(StringRef Op) const {
    return Op == "=" || Op == "+=" || Op == "-=" || Op == "*=" || Op == "/=" ||
           Op == "%=" || Op == "<<=" || Op == ">>=" || Op == "&=" || Op == "^=" ||
           Op == "|=";
  }

  Expr parseAssignment() {
    size_t B = cur().B;
    Expr L = parseConditional();
    if (cur().K == ExprTok::Op && isAssignOp(cur().Text)) {
      StringRef Op = cur().Text;
      adv();
      Expr R = parseAssignment();
      return makeNode(ExprKind::Assignment, B, R.Text.empty() ? cur().E : Tokens[I-1].E,
                      {std::move(L), std::move(R)}, Op);
    }
    return L;
  }

  Expr parseConditional() {
    size_t B = cur().B;
    Expr C = parseBinary(1);
    if (cur().K == ExprTok::Question) {
      adv();
      Expr T = parseAssignment();
      if (cur().K == ExprTok::Colon) adv();
      Expr E = parseConditional();
      return makeNode(ExprKind::Conditional, B, Tokens[I-1].E, {std::move(C), std::move(T), std::move(E)});
    }
    return C;
  }

  Expr parseBinary(int MinPrec) {
    Expr L = parseUnary();
    while (cur().K == ExprTok::Op) {
      int P = precedence(cur().Text);
      if (P < MinPrec) break;
      StringRef Op = cur().Text;
      size_t B = Tokens[I].B;
      adv();
      Expr R = parseBinary(P + 1);
      size_t Start = L.Text.empty() ? B : 0;
      (void)Start;
      L = makeNode(ExprKind::Binary, Tokens[I==0?0:I-1].B, Tokens[I-1].E,
                   {std::move(L), std::move(R)}, Op);
    }
    return L;
  }

  bool isTypeCastPattern() {
    if (cur().K != ExprTok::LParen)
      return false;
    size_t J = I + 1;
    bool HasIdent = false;
    int Depth = 1;
    while (J < Tokens.size()) {
      auto K = Tokens[J].K;
      if (K == ExprTok::LParen) ++Depth;
      else if (K == ExprTok::RParen) { --Depth; if (Depth == 0) break; }
      if (K == ExprTok::Ident) HasIdent = true;
      if (!(K == ExprTok::Ident || K == ExprTok::Number || K == ExprTok::Op ||
            K == ExprTok::LParen || K == ExprTok::RParen || K == ExprTok::LBracket ||
            K == ExprTok::RBracket || K == ExprTok::Comma))
        return false;
      ++J;
    }
    return HasIdent && J < Tokens.size() - 1;
  }

  Expr parseUnary() {
    if (cur().K == ExprTok::Op && (cur().Text == "+" || cur().Text == "-" ||
        cur().Text == "!" || cur().Text == "~" || cur().Text == "++" || cur().Text == "--")) {
      size_t B = cur().B;
      StringRef Op = cur().Text;
      adv();
      Expr X = parseUnary();
      return makeNode(ExprKind::Unary, B, Tokens[I-1].E, {std::move(X)}, Op);
    }
    if (isTypeCastPattern()) {
      size_t B = cur().B;
      adv();
      size_t TypeB = cur().B;
      int Depth = 1;
      while (I < Tokens.size()) {
        if (cur().K == ExprTok::LParen) ++Depth;
        else if (cur().K == ExprTok::RParen) { --Depth; if (Depth == 0) break; }
        adv();
      }
      size_t TypeE = cur().B;
      if (cur().K == ExprTok::RParen) adv();
      Expr X = parseUnary();
      return makeNode(ExprKind::Cast, B, Tokens[I-1].E, {std::move(X)}, "", text(TypeB, TypeE));
    }
    return parsePostfix();
  }

  Expr parsePostfix() {
    Expr Base = parsePrimary();
    while (true) {
      if (cur().K == ExprTok::LBracket) {
        size_t B = cur().B;
        adv();
        Expr Idx = parseAssignment();
        std::vector<Expr> Ch;
        Ch.push_back(std::move(Base));
        Ch.push_back(std::move(Idx));
        if (cur().K == ExprTok::Colon) {
          adv();
          Ch.push_back(parseAssignment());
        }
        if (cur().K == ExprTok::RBracket) adv();
        Base = makeNode(ExprKind::Index, B, Tokens[I-1].E, std::move(Ch));
        continue;
      }
      if (cur().K == ExprTok::LParen) {
        size_t B = cur().B;
        adv();
        std::vector<Expr> Ch;
        Ch.push_back(std::move(Base));
        if (cur().K != ExprTok::RParen) {
          while (true) {
            Ch.push_back(parseAssignment());
            if (cur().K != ExprTok::Comma) break;
            adv();
          }
        }
        if (cur().K == ExprTok::RParen) adv();
        Base = makeNode(ExprKind::Call, B, Tokens[I-1].E, std::move(Ch));
        continue;
      }
      if (cur().K == ExprTok::Dot) {
        size_t B = cur().B;
        adv();
        std::string Member;
        if (cur().K == ExprTok::Ident) { Member = cur().Text.str(); adv(); }
        Base = makeNode(ExprKind::Member, B, Tokens[I-1].E, {std::move(Base)}, ".", Member);
        continue;
      }
      if (cur().K == ExprTok::Op && (cur().Text == "++" || cur().Text == "--")) {
        StringRef Op = cur().Text;
        size_t B = cur().B;
        adv();
        Base = makeNode(ExprKind::Postfix, B, Tokens[I-1].E, {std::move(Base)}, Op);
        continue;
      }
      break;
    }
    return Base;
  }

  Expr parsePrimary() {
    if (cur().K == ExprTok::Ident) {
      Expr R = makeNode(ExprKind::Identifier, cur().B, cur().E, {}, "", cur().Text);
      adv();
      return R;
    }
    if (cur().K == ExprTok::Number || cur().K == ExprTok::String) {
      Expr R = makeNode(ExprKind::Literal, cur().B, cur().E, {}, "", cur().Text);
      adv();
      return R;
    }
    if (cur().K == ExprTok::LParen) {
      size_t B = cur().B;
      adv();
      Expr In = parseAssignment();
      if (cur().K == ExprTok::RParen)
        adv();
      return makeNode(ExprKind::Group, B, Tokens[I-1].E, {std::move(In)});
    }
    Expr R = makeNode(ExprKind::Unknown, cur().B, cur().E, {}, "", cur().Text);
    adv();
    return R;
  }
};

Expr parseExpressionAST(StringRef Text) {
  ExprParser P(Text);
  return P.parse();
}

} // namespace

class Parser::Impl {
public:
  explicit Impl(StringRef Buffer) : Buffer(Buffer), Lex(Buffer), Cur(Lex.next()) {}
  Expected<Description> parseDescription();

private:
  StringRef Buffer;
  Lexer Lex;
  Token Cur;
  std::vector<std::string> Diags;

  void advance() { Cur = Lex.next(); }
  void addDiag(const Twine &Message) {
    Diags.push_back(formatv("{0}:{1}: {2}", Cur.Loc.Line, Cur.Loc.Column,
                            Message.str()).str());
  }
  bool consume(TokenKind Kind, const Twine &Err);
  std::optional<std::string> parseIdentifier(const Twine &Err);
  std::optional<std::string> parseImport();
  std::optional<InstructionSetDef> parseInstructionSet();
  std::optional<CoreDef> parseCore();
  bool parseISA(ISASections &Sections);
  std::vector<std::string> parseAttributes();
  std::optional<std::string> parseSingleAttribute();
  std::optional<Instruction> parseInstruction();
  std::optional<Assembly> parseAssembly();
  std::optional<std::vector<EncodingField>> parseEncoding();
  std::optional<EncodingField> parseEncodingField();
  std::optional<AlwaysBlock> parseAlwaysBlock();
  bool isIdentifier(StringRef Text) const {
    return Cur.Kind == TokenKind::Identifier && Cur.Lexeme == Text;
  }
  std::optional<std::string> consumeParenthesizedExpressionText();
  std::optional<std::string> consumeUntilSemicolonText();
  std::optional<Statement> parseSwitchSection();
  std::optional<Statement> parseStatement();
  std::optional<std::string> parseRawBracedBlock();
  static std::string stripQuotes(StringRef S);
  Error fail();
};

Parser::Parser(StringRef Buffer) : PImpl(std::make_unique<Impl>(Buffer)) {}

Parser::~Parser() = default;

Expected<Description> Parser::parseDescription() { return PImpl->parseDescription(); }

bool Parser::Impl::consume(TokenKind Kind, const Twine &Err) {
  if (Cur.Kind != Kind) {
    addDiag(Err);
    return false;
  }
  advance();
  return true;
}

std::optional<std::string> Parser::Impl::parseIdentifier(const Twine &Err) {
  if (Cur.Kind != TokenKind::Identifier) {
    addDiag(Err);
    return std::nullopt;
  }
  std::string Text = Cur.Lexeme.str();
  advance();
  return Text;
}

std::optional<std::string> Parser::Impl::parseImport() {
  if (!consume(TokenKind::KwImport, "expected 'import'"))
    return std::nullopt;
  if (Cur.Kind != TokenKind::String) {
    addDiag("expected import string literal");
    return std::nullopt;
  }
  std::string Text = stripQuotes(Cur.Lexeme);
  advance();
  return Text;
}

Expected<Description> Parser::Impl::parseDescription() {
  Description D;
  while (Cur.Kind == TokenKind::KwImport) {
    auto I = parseImport();
    if (!I)
      return fail();
    D.Imports.push_back(std::move(*I));
  }
  while (Cur.Kind != TokenKind::Eof) {
    if (Cur.Kind == TokenKind::KwInstructionSet) {
      auto S = parseInstructionSet();
      if (!S)
        return fail();
      D.InstructionSets.push_back(std::move(*S));
    } else if (Cur.Kind == TokenKind::KwCore) {
      auto C = parseCore();
      if (!C)
        return fail();
      D.Cores.push_back(std::move(*C));
    } else {
      addDiag("expected 'InstructionSet' or 'Core'");
      return fail();
    }
  }
  if (!Diags.empty())
    return fail();
  return D;
}

std::optional<InstructionSetDef> Parser::Impl::parseInstructionSet() {
  if (!consume(TokenKind::KwInstructionSet, "expected 'InstructionSet'"))
    return std::nullopt;
  auto Name = parseIdentifier("expected instruction set name");
  if (!Name)
    return std::nullopt;
  InstructionSetDef Set;
  Set.Name = *Name;
  if (Cur.Kind == TokenKind::KwCombines) {
    advance();
    auto First = parseIdentifier("expected instruction set name after combines");
    if (!First)
      return std::nullopt;
    Set.Combines.push_back(*First);
    while (Cur.Kind == TokenKind::Comma) {
      advance();
      auto Next = parseIdentifier("expected instruction set name after ','");
      if (!Next)
        return std::nullopt;
      Set.Combines.push_back(*Next);
    }
    if (!consume(TokenKind::Semicolon, "expected ';' after combines clause"))
      return std::nullopt;
    return Set;
  }
  if (Cur.Kind == TokenKind::KwExtends) {
    advance();
    auto Base = parseIdentifier("expected base instruction set name");
    if (!Base)
      return std::nullopt;
    Set.Extends = *Base;
  }
  if (!consume(TokenKind::LBrace, "expected '{' before ISA body"))
    return std::nullopt;
  if (!parseISA(Set.ISA))
    return std::nullopt;
  if (!consume(TokenKind::RBrace, "expected '}' after ISA body"))
    return std::nullopt;
  return Set;
}

std::optional<CoreDef> Parser::Impl::parseCore() {
  if (!consume(TokenKind::KwCore, "expected 'Core'"))
    return std::nullopt;
  auto Name = parseIdentifier("expected core name");
  if (!Name)
    return std::nullopt;
  CoreDef Core;
  Core.Name = *Name;
  if (Cur.Kind == TokenKind::KwProvides) {
    advance();
    auto First = parseIdentifier("expected instruction set name after provides");
    if (!First)
      return std::nullopt;
    Core.Provides.push_back(*First);
    while (Cur.Kind == TokenKind::Comma) {
      advance();
      auto Next = parseIdentifier("expected instruction set name after ','");
      if (!Next)
        return std::nullopt;
      Core.Provides.push_back(*Next);
    }
  }
  if (!consume(TokenKind::LBrace, "expected '{' before ISA body"))
    return std::nullopt;
  if (!parseISA(Core.ISA))
    return std::nullopt;
  if (!consume(TokenKind::RBrace, "expected '}' after ISA body"))
    return std::nullopt;
  return Core;
}

bool Parser::Impl::parseISA(ISASections &Sections) {
  bool SeenArch = false, SeenFunctions = false, SeenInstructions = false,
       SeenAlways = false;
  while (Cur.Kind != TokenKind::RBrace && Cur.Kind != TokenKind::Eof) {
    if (Cur.Kind == TokenKind::KwArchitecturalState) {
      if (SeenArch) {
        addDiag("duplicate 'architectural_state' section");
        return false;
      }
      SeenArch = true;
      advance();
      auto Raw = parseRawBracedBlock();
      if (!Raw)
        return false;
      Sections.ArchitecturalState = *Raw;
      continue;
    }
    if (Cur.Kind == TokenKind::KwFunctions) {
      if (SeenFunctions) {
        addDiag("duplicate 'functions' section");
        return false;
      }
      SeenFunctions = true;
      advance();
      auto Raw = parseRawBracedBlock();
      if (!Raw)
        return false;
      Sections.Functions = *Raw;
      continue;
    }
    if (Cur.Kind == TokenKind::KwInstructions) {
      if (SeenInstructions) {
        addDiag("duplicate 'instructions' section");
        return false;
      }
      SeenInstructions = true;
      advance();
      Sections.CommonInstructionAttributes = parseAttributes();
      if (!consume(TokenKind::LBrace, "expected '{' after instructions"))
        return false;
      while (Cur.Kind != TokenKind::RBrace && Cur.Kind != TokenKind::Eof) {
        auto Inst = parseInstruction();
        if (!Inst)
          return false;
        Sections.Instructions.push_back(std::move(*Inst));
      }
      if (!consume(TokenKind::RBrace, "expected '}' after instructions section"))
        return false;
      continue;
    }
    if (Cur.Kind == TokenKind::KwAlways) {
      if (SeenAlways) {
        addDiag("duplicate 'always' section");
        return false;
      }
      SeenAlways = true;
      advance();
      Sections.CommonAlwaysAttributes = parseAttributes();
      if (!consume(TokenKind::LBrace, "expected '{' after always"))
        return false;
      while (Cur.Kind != TokenKind::RBrace && Cur.Kind != TokenKind::Eof) {
        auto B = parseAlwaysBlock();
        if (!B)
          return false;
        Sections.AlwaysBlocks.push_back(std::move(*B));
      }
      if (!consume(TokenKind::RBrace, "expected '}' after always section"))
        return false;
      continue;
    }
    addDiag("unexpected token in ISA body");
    return false;
  }
  return true;
}

std::vector<std::string> Parser::Impl::parseAttributes() {
  std::vector<std::string> Attrs;
  while (Cur.Kind == TokenKind::DoubleLBracket) {
    auto A = parseSingleAttribute();
    if (!A)
      break;
    Attrs.push_back(std::move(*A));
  }
  return Attrs;
}

std::optional<std::string> Parser::Impl::parseSingleAttribute() {
  size_t Start = Cur.Loc.Offset;
  int Depth = 0;
  while (Cur.Kind != TokenKind::Eof) {
    if (Cur.Kind == TokenKind::DoubleLBracket)
      ++Depth;
    else if (Cur.Kind == TokenKind::DoubleRBracket)
      --Depth;
    size_t End = Cur.EndOffset;
    advance();
    if (Depth == 0)
      return Buffer.slice(Start, End).str();
    if (Depth < 0)
      break;
  }
  addDiag("unterminated attribute");
  return std::nullopt;
}

std::optional<Instruction> Parser::Impl::parseInstruction() {
  auto Name = parseIdentifier("expected instruction name");
  if (!Name)
    return std::nullopt;
  Instruction Inst;
  Inst.Name = *Name;
  Inst.Attributes = parseAttributes();
  if (!consume(TokenKind::LBrace, "expected '{' in instruction definition"))
    return std::nullopt;
  if (!consume(TokenKind::KwEncoding, "expected 'encoding' clause") ||
      !consume(TokenKind::Colon, "expected ':' after encoding"))
    return std::nullopt;
  auto Enc = parseEncoding();
  if (!Enc)
    return std::nullopt;
  Inst.Encoding = std::move(*Enc);
  if (!consume(TokenKind::Semicolon, "expected ';' after encoding"))
    return std::nullopt;
  if (Cur.Kind == TokenKind::KwAssembly) {
    auto Asm = parseAssembly();
    if (!Asm)
      return std::nullopt;
    Inst.Asm = std::move(*Asm);
  }
  if (!consume(TokenKind::KwBehavior, "expected 'behavior' clause") ||
      !consume(TokenKind::Colon, "expected ':' after behavior"))
    return std::nullopt;
  auto Behavior = parseStatement();
  if (!Behavior)
    return std::nullopt;
  Inst.Behavior = std::move(*Behavior);
  if (!consume(TokenKind::RBrace, "expected '}' after instruction"))
    return std::nullopt;
  return Inst;
}

std::optional<Assembly> Parser::Impl::parseAssembly() {
  if (!consume(TokenKind::KwAssembly, "expected 'assembly'") ||
      !consume(TokenKind::Colon, "expected ':' after assembly"))
    return std::nullopt;
  Assembly Asm;
  if (Cur.Kind == TokenKind::String) {
    Asm.Template = stripQuotes(Cur.Lexeme);
    advance();
  } else if (Cur.Kind == TokenKind::LBrace) {
    Asm.IsStructured = true;
    advance();
    if (Cur.Kind != TokenKind::String) {
      addDiag("expected mnemonic string in assembly block");
      return std::nullopt;
    }
    Asm.Mnemonic = stripQuotes(Cur.Lexeme);
    advance();
    if (!consume(TokenKind::Comma, "expected ',' after mnemonic") ||
        Cur.Kind != TokenKind::String) {
      addDiag("expected assembly template string");
      return std::nullopt;
    }
    Asm.Template = stripQuotes(Cur.Lexeme);
    advance();
    if (!consume(TokenKind::RBrace, "expected '}' after assembly block"))
      return std::nullopt;
  } else {
    addDiag("expected assembly string or '{...}' form");
    return std::nullopt;
  }
  if (!consume(TokenKind::Semicolon, "expected ';' after assembly clause"))
    return std::nullopt;
  return Asm;
}

std::optional<std::vector<EncodingField>> Parser::Impl::parseEncoding() {
  std::vector<EncodingField> Fields;
  while (true) {
    auto F = parseEncodingField();
    if (!F)
      return std::nullopt;
    Fields.push_back(std::move(*F));
    if (Cur.Kind != TokenKind::DoubleColon)
      break;
    advance();
  }
  return Fields;
}

std::optional<EncodingField> Parser::Impl::parseEncodingField() {
  if (Cur.Kind == TokenKind::Integer) {
    EncodingField F;
    F.IsBitValue = true;
    F.Value = Cur.Lexeme.str();
    advance();
    return F;
  }
  auto Name = parseIdentifier("expected encoding field name or literal");
  if (!Name || !consume(TokenKind::LBracket, "expected '[' after encoding field name"))
    return std::nullopt;
  if (Cur.Kind != TokenKind::Integer) {
    addDiag("expected start bit literal");
    return std::nullopt;
  }
  std::string StartBit = Cur.Lexeme.str();
  advance();
  if (!consume(TokenKind::Colon, "expected ':' in bit range"))
    return std::nullopt;
  if (Cur.Kind != TokenKind::Integer) {
    addDiag("expected end bit literal");
    return std::nullopt;
  }
  std::string EndBit = Cur.Lexeme.str();
  advance();
  if (!consume(TokenKind::RBracket, "expected ']' after bit range"))
    return std::nullopt;
  EncodingField F;
  F.Name = *Name;
  F.StartBit = std::move(StartBit);
  F.EndBit = std::move(EndBit);
  return F;
}

std::optional<AlwaysBlock> Parser::Impl::parseAlwaysBlock() {
  auto Name = parseIdentifier("expected always block name");
  if (!Name)
    return std::nullopt;
  AlwaysBlock Block;
  Block.Name = *Name;
  Block.Attributes = parseAttributes();
  if (Cur.Kind != TokenKind::LBrace) {
    addDiag("expected compound statement for always block behavior");
    return std::nullopt;
  }
  auto Body = parseStatement();
  if (!Body)
    return std::nullopt;
  Block.Behavior = std::move(*Body);
  return Block;
}

std::optional<std::string> Parser::Impl::consumeParenthesizedExpressionText() {
  size_t Start = Cur.Loc.Offset;
  if (!consume(TokenKind::LParen, "expected '('"))
    return std::nullopt;
  int Depth = 1;
  while (Cur.Kind != TokenKind::Eof) {
    if (Cur.Kind == TokenKind::LParen)
      ++Depth;
    else if (Cur.Kind == TokenKind::RParen)
      --Depth;
    advance();
    if (Depth == 0)
      return Buffer.slice(Start, Cur.Loc.Offset).trim().str();
  }
  addDiag("unterminated parenthesized expression");
  return std::nullopt;
}

std::optional<std::string> Parser::Impl::consumeUntilSemicolonText() {
  size_t Start = Cur.Loc.Offset;
  int ParenDepth = 0, BracketDepth = 0, BraceDepth = 0;
  while (Cur.Kind != TokenKind::Eof) {
    if (Cur.Kind == TokenKind::LParen)
      ++ParenDepth;
    else if (Cur.Kind == TokenKind::RParen)
      --ParenDepth;
    else if (Cur.Kind == TokenKind::LBracket)
      ++BracketDepth;
    else if (Cur.Kind == TokenKind::RBracket)
      --BracketDepth;
    else if (Cur.Kind == TokenKind::LBrace)
      ++BraceDepth;
    else if (Cur.Kind == TokenKind::RBrace)
      --BraceDepth;
    if (Cur.Kind == TokenKind::Semicolon && ParenDepth == 0 && BracketDepth == 0 &&
        BraceDepth == 0) {
      advance();
      return Buffer.slice(Start, Cur.Loc.Offset).trim().str();
    }
    advance();
  }
  addDiag("expected ';' to terminate statement");
  return std::nullopt;
}

std::optional<Statement> Parser::Impl::parseSwitchSection() {
  size_t Start = Cur.Loc.Offset;
  StatementKind Kind = StatementKind::Case;
  if (isIdentifier("case")) {
    Kind = StatementKind::Case;
    advance();
    int ParenDepth = 0, BracketDepth = 0, BraceDepth = 0;
    while (Cur.Kind != TokenKind::Eof) {
      if (Cur.Kind == TokenKind::LParen)
        ++ParenDepth;
      else if (Cur.Kind == TokenKind::RParen)
        --ParenDepth;
      else if (Cur.Kind == TokenKind::LBracket)
        ++BracketDepth;
      else if (Cur.Kind == TokenKind::RBracket)
        --BracketDepth;
      else if (Cur.Kind == TokenKind::LBrace)
        ++BraceDepth;
      else if (Cur.Kind == TokenKind::RBrace)
        --BraceDepth;
      if (Cur.Kind == TokenKind::Colon && ParenDepth == 0 && BracketDepth == 0 &&
          BraceDepth == 0) {
        advance();
        break;
      }
      advance();
    }
    if (Cur.Kind == TokenKind::Eof) {
      addDiag("unterminated switch case label");
      return std::nullopt;
    }
  } else if (isIdentifier("default")) {
    Kind = StatementKind::Default;
    advance();
    if (!consume(TokenKind::Colon, "expected ':' after default"))
      return std::nullopt;
  } else {
    addDiag("expected 'case' or 'default' in switch");
    return std::nullopt;
  }
  std::vector<Statement> Children;
  while (Cur.Kind != TokenKind::Eof && Cur.Kind != TokenKind::RBrace &&
         !isIdentifier("case") && !isIdentifier("default")) {
    auto Stmt = parseStatement();
    if (!Stmt)
      return std::nullopt;
    Children.push_back(std::move(*Stmt));
  }
  return Statement{Kind, Buffer.slice(Start, Cur.Loc.Offset).trim().str(), {},
                   std::move(Children)};
}

std::optional<Statement> Parser::Impl::parseStatement() {
  size_t Start = Cur.Loc.Offset;
  if (Cur.Kind == TokenKind::Semicolon) {
    advance();
    return Statement{StatementKind::Empty, Buffer.slice(Start, Cur.Loc.Offset).trim().str(), {}, {}};
  }
  if (Cur.Kind == TokenKind::LBrace) {
    std::vector<Statement> Children;
    advance();
    while (Cur.Kind != TokenKind::RBrace && Cur.Kind != TokenKind::Eof) {
      auto Stmt = parseStatement();
      if (!Stmt)
        return std::nullopt;
      Children.push_back(std::move(*Stmt));
    }
    if (!consume(TokenKind::RBrace, "expected '}' to close compound statement"))
      return std::nullopt;
    return Statement{StatementKind::Compound, Buffer.slice(Start, Cur.Loc.Offset).trim().str(), {},
                     std::move(Children)};
  }
  if (isIdentifier("if")) {
    advance();
    auto Cond = consumeParenthesizedExpressionText();
    if (!Cond)
      return std::nullopt;
    auto ThenStmt = parseStatement();
    if (!ThenStmt)
      return std::nullopt;
    std::vector<Statement> Children;
    Children.push_back(std::move(*ThenStmt));
    if (isIdentifier("else")) {
      advance();
      auto ElseStmt = parseStatement();
      if (!ElseStmt)
        return std::nullopt;
      Children.push_back(std::move(*ElseStmt));
    }
    return Statement{StatementKind::If, Buffer.slice(Start, Cur.Loc.Offset).trim().str(),
                     {parseExpressionAST(*Cond)}, std::move(Children)};
  }
  if (isIdentifier("switch")) {
    std::vector<Statement> Children;
    advance();
    auto Cond = consumeParenthesizedExpressionText();
    if (!Cond)
      return std::nullopt;
    if (!consume(TokenKind::LBrace, "expected '{' after switch condition"))
      return std::nullopt;
    while (Cur.Kind != TokenKind::RBrace && Cur.Kind != TokenKind::Eof) {
      auto Section = parseSwitchSection();
      if (!Section)
        return std::nullopt;
      Children.push_back(std::move(*Section));
    }
    if (!consume(TokenKind::RBrace, "expected '}' to close switch"))
      return std::nullopt;
    return Statement{StatementKind::Switch, Buffer.slice(Start, Cur.Loc.Offset).trim().str(),
                     {parseExpressionAST(*Cond)}, std::move(Children)};
  }
  if (isIdentifier("while")) {
    advance();
    auto Cond = consumeParenthesizedExpressionText();
    if (!Cond)
      return std::nullopt;
    auto Body = parseStatement();
    if (!Body)
      return std::nullopt;
    return Statement{StatementKind::While, Buffer.slice(Start, Cur.Loc.Offset).trim().str(),
                     {parseExpressionAST(*Cond)}, {*Body}};
  }
  if (isIdentifier("for")) {
    advance();
    auto Header = consumeParenthesizedExpressionText();
    if (!Header)
      return std::nullopt;
    auto Body = parseStatement();
    if (!Body)
      return std::nullopt;
    return Statement{StatementKind::For, Buffer.slice(Start, Cur.Loc.Offset).trim().str(),
                     {parseExpressionAST(*Header)}, {*Body}};
  }
  if (isIdentifier("do")) {
    advance();
    auto Body = parseStatement();
    if (!Body)
      return std::nullopt;
    if (!isIdentifier("while")) {
      addDiag("expected 'while' after do-body");
      return std::nullopt;
    }
    advance();
    auto Cond = consumeParenthesizedExpressionText();
    if (!Cond)
      return std::nullopt;
    if (!consume(TokenKind::Semicolon, "expected ';' after do-while"))
      return std::nullopt;
    return Statement{StatementKind::DoWhile, Buffer.slice(Start, Cur.Loc.Offset).trim().str(),
                     {parseExpressionAST(*Cond)}, {*Body}};
  }
  if (isIdentifier("spawn")) {
    advance();
    auto Body = parseStatement();
    if (!Body)
      return std::nullopt;
    return Statement{StatementKind::Spawn, Buffer.slice(Start, Cur.Loc.Offset).trim().str(), {},
                     {*Body}};
  }
  if (isIdentifier("continue") || isIdentifier("break")) {
    StatementKind Kind = isIdentifier("continue") ? StatementKind::Continue : StatementKind::Break;
    advance();
    if (!consume(TokenKind::Semicolon, "expected ';' after jump statement"))
      return std::nullopt;
    return Statement{Kind, Buffer.slice(Start, Cur.Loc.Offset).trim().str(), {},
                     {}};
  }
  if (isIdentifier("return")) {
    advance();
    if (Cur.Kind == TokenKind::Semicolon) {
      advance();
      return Statement{StatementKind::Return, Buffer.slice(Start, Cur.Loc.Offset).trim().str(), {},
                       {}};
    }
    auto ExprText = consumeUntilSemicolonText();
    if (!ExprText)
      return std::nullopt;
    return Statement{StatementKind::Return, Buffer.slice(Start, Cur.Loc.Offset).trim().str(),
                     {parseExpressionAST(*ExprText)}, {}};
  }
  auto ExprText = consumeUntilSemicolonText();
  if (!ExprText)
    return std::nullopt;
  return Statement{StatementKind::Expression, Buffer.slice(Start, Cur.Loc.Offset).trim().str(),
                   {parseExpressionAST(*ExprText)}, {}};
}

std::optional<std::string> Parser::Impl::parseRawBracedBlock() {
  if (Cur.Kind != TokenKind::LBrace) {
    addDiag("expected '{'");
    return std::nullopt;
  }
  size_t InnerStart = Cur.EndOffset;
  int Depth = 0;
  while (Cur.Kind != TokenKind::Eof) {
    if (Cur.Kind == TokenKind::LBrace)
      ++Depth;
    else if (Cur.Kind == TokenKind::RBrace)
      --Depth;
    size_t End = Cur.Loc.Offset;
    advance();
    if (Depth == 0)
      return Buffer.slice(InnerStart, End).str();
  }
  addDiag("unterminated '{...}' block");
  return std::nullopt;
}

std::string Parser::Impl::stripQuotes(StringRef S) {
  if (S.size() >= 2 && S.front() == '"' && S.back() == '"')
    return S.slice(1, S.size() - 1).str();
  return S.str();
}

Error Parser::Impl::fail() {
  std::string Joined;
  raw_string_ostream OS(Joined);
  for (StringRef D : Diags)
    OS << D << '\n';
  return createStringError(inconvertibleErrorCode(), OS.str());
}
