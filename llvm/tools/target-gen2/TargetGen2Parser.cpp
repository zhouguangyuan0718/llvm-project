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
  bool consumeParenthesizedExpression();
  bool consumeUntilSemicolon();
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

bool Parser::Impl::consumeParenthesizedExpression() {
  if (!consume(TokenKind::LParen, "expected '('"))
    return false;
  int Depth = 1;
  while (Cur.Kind != TokenKind::Eof) {
    if (Cur.Kind == TokenKind::LParen)
      ++Depth;
    else if (Cur.Kind == TokenKind::RParen)
      --Depth;
    advance();
    if (Depth == 0)
      return true;
  }
  addDiag("unterminated parenthesized expression");
  return false;
}

bool Parser::Impl::consumeUntilSemicolon() {
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
      return true;
    }
    advance();
  }
  addDiag("expected ';' to terminate statement");
  return false;
}

std::optional<Statement> Parser::Impl::parseSwitchSection() {
  size_t Start = Cur.Loc.Offset;
  std::string Kind;
  if (isIdentifier("case")) {
    Kind = "case";
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
    Kind = "default";
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
  return Statement{Kind, Buffer.slice(Start, Cur.Loc.Offset).trim().str(),
                   std::move(Children)};
}

std::optional<Statement> Parser::Impl::parseStatement() {
  size_t Start = Cur.Loc.Offset;
  if (Cur.Kind == TokenKind::Semicolon) {
    advance();
    return Statement{"empty", Buffer.slice(Start, Cur.Loc.Offset).trim().str(), {}};
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
    return Statement{"compound", Buffer.slice(Start, Cur.Loc.Offset).trim().str(),
                     std::move(Children)};
  }
  if (isIdentifier("if")) {
    advance();
    if (!consumeParenthesizedExpression())
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
    return Statement{"if", Buffer.slice(Start, Cur.Loc.Offset).trim().str(),
                     std::move(Children)};
  }
  if (isIdentifier("switch")) {
    std::vector<Statement> Children;
    advance();
    if (!consumeParenthesizedExpression())
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
    return Statement{"switch", Buffer.slice(Start, Cur.Loc.Offset).trim().str(),
                     std::move(Children)};
  }
  if (isIdentifier("while")) {
    advance();
    if (!consumeParenthesizedExpression())
      return std::nullopt;
    auto Body = parseStatement();
    if (!Body)
      return std::nullopt;
    return Statement{"while", Buffer.slice(Start, Cur.Loc.Offset).trim().str(), {*Body}};
  }
  if (isIdentifier("for")) {
    advance();
    if (!consumeParenthesizedExpression())
      return std::nullopt;
    auto Body = parseStatement();
    if (!Body)
      return std::nullopt;
    return Statement{"for", Buffer.slice(Start, Cur.Loc.Offset).trim().str(), {*Body}};
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
    if (!consumeParenthesizedExpression())
      return std::nullopt;
    if (!consume(TokenKind::Semicolon, "expected ';' after do-while"))
      return std::nullopt;
    return Statement{"do-while", Buffer.slice(Start, Cur.Loc.Offset).trim().str(),
                     {*Body}};
  }
  if (isIdentifier("spawn")) {
    advance();
    auto Body = parseStatement();
    if (!Body)
      return std::nullopt;
    return Statement{"spawn", Buffer.slice(Start, Cur.Loc.Offset).trim().str(), {*Body}};
  }
  if (isIdentifier("continue") || isIdentifier("break")) {
    std::string Kind = Cur.Lexeme.str();
    advance();
    if (!consume(TokenKind::Semicolon, "expected ';' after jump statement"))
      return std::nullopt;
    return Statement{std::move(Kind), Buffer.slice(Start, Cur.Loc.Offset).trim().str(), {}};
  }
  if (isIdentifier("return")) {
    advance();
    if (Cur.Kind == TokenKind::Semicolon) {
      advance();
      return Statement{"return", Buffer.slice(Start, Cur.Loc.Offset).trim().str(), {}};
    }
    if (!consumeUntilSemicolon())
      return std::nullopt;
    return Statement{"return", Buffer.slice(Start, Cur.Loc.Offset).trim().str(), {}};
  }
  if (!consumeUntilSemicolon())
    return std::nullopt;
  return Statement{"expression", Buffer.slice(Start, Cur.Loc.Offset).trim().str(), {}};
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
