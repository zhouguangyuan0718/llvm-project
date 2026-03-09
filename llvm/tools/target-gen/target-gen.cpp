//===- target-gen.cpp - CoreDSL frontend entry point ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This tool implements a lightweight compilation frontend for a CoreDSL-inspired
// language and emits a normalized JSON representation.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace {

struct SourceLocation {
  size_t Offset = 0;
  unsigned Line = 1;
  unsigned Column = 1;
};

enum class TokenKind {
  Eof,
  Identifier,
  Number,
  String,

  LBrace,
  RBrace,
  LParen,
  RParen,
  Semicolon,
  Comma,
  Equal,

  KwCore,
  KwRegister,
  KwInstruction,
  KwField,
  KwEncoding,
  KwReset,
  KwWidth,
};

struct Token {
  TokenKind Kind = TokenKind::Eof;
  StringRef Lexeme;
  SourceLocation Loc;
};

struct Lexer {
  explicit Lexer(StringRef Buffer) : Buffer(Buffer) {}

  Token next() {
    skipTrivia();
    if (AtEnd)
      return makeToken(TokenKind::Eof, "");

    char C = peek();
    if (isalpha(static_cast<unsigned char>(C)) || C == '_')
      return lexIdentifierOrKeyword();
    if (isdigit(static_cast<unsigned char>(C)))
      return lexNumber();
    if (C == '"')
      return lexString();

    SourceLocation Start = Loc;
    advance();
    switch (C) {
    case '{':
      return Token{TokenKind::LBrace, "{", Start};
    case '}':
      return Token{TokenKind::RBrace, "}", Start};
    case '(':
      return Token{TokenKind::LParen, "(", Start};
    case ')':
      return Token{TokenKind::RParen, ")", Start};
    case ';':
      return Token{TokenKind::Semicolon, ";", Start};
    case ',':
      return Token{TokenKind::Comma, ",", Start};
    case '=':
      return Token{TokenKind::Equal, "=", Start};
    default:
      Diags.push_back(formatv("{0}:{1}: unknown character '{2}'", Start.Line,
                              Start.Column, C)
                          .str());
      return next();
    }
  }

  std::vector<std::string> Diags;

private:
  StringRef Buffer;
  size_t Index = 0;
  SourceLocation Loc;
  bool AtEnd = false;

  char peek() const { return Buffer[Index]; }

  void advance() {
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

  Token makeToken(TokenKind Kind, StringRef Lexeme) const {
    return Token{Kind, Lexeme, Loc};
  }

  void skipTrivia() {
    while (!AtEnd) {
      if (isspace(static_cast<unsigned char>(peek()))) {
        advance();
        continue;
      }

      if (peek() == '#') {
        while (!AtEnd && peek() != '\n')
          advance();
        continue;
      }

      if (peek() == '/' && Index + 1 < Buffer.size() && Buffer[Index + 1] == '/') {
        advance();
        advance();
        while (!AtEnd && peek() != '\n')
          advance();
        continue;
      }
      break;
    }
  }

  Token lexIdentifierOrKeyword() {
    SourceLocation Start = Loc;
    size_t StartIdx = Index;
    while (!AtEnd &&
           (isalnum(static_cast<unsigned char>(peek())) || peek() == '_'))
      advance();
    StringRef Lexeme = Buffer.slice(StartIdx, Index);

    if (Lexeme == "core")
      return Token{TokenKind::KwCore, Lexeme, Start};
    if (Lexeme == "register")
      return Token{TokenKind::KwRegister, Lexeme, Start};
    if (Lexeme == "instruction")
      return Token{TokenKind::KwInstruction, Lexeme, Start};
    if (Lexeme == "field")
      return Token{TokenKind::KwField, Lexeme, Start};
    if (Lexeme == "encoding")
      return Token{TokenKind::KwEncoding, Lexeme, Start};
    if (Lexeme == "reset")
      return Token{TokenKind::KwReset, Lexeme, Start};
    if (Lexeme == "width")
      return Token{TokenKind::KwWidth, Lexeme, Start};
    return Token{TokenKind::Identifier, Lexeme, Start};
  }

  Token lexNumber() {
    SourceLocation Start = Loc;
    size_t StartIdx = Index;
    if (!AtEnd && peek() == '0' && Index + 1 < Buffer.size() &&
        (Buffer[Index + 1] == 'x' || Buffer[Index + 1] == 'X')) {
      advance();
      advance();
      while (!AtEnd && isxdigit(static_cast<unsigned char>(peek())))
        advance();
    } else {
      while (!AtEnd && isdigit(static_cast<unsigned char>(peek())))
        advance();
    }
    return Token{TokenKind::Number, Buffer.slice(StartIdx, Index), Start};
  }

  Token lexString() {
    SourceLocation Start = Loc;
    advance();
    size_t StartIdx = Index;
    while (!AtEnd && peek() != '"')
      advance();
    StringRef Raw = Buffer.slice(StartIdx, Index);
    if (!AtEnd)
      advance();
    else
      Diags.push_back(
          formatv("{0}:{1}: unterminated string literal", Start.Line, Start.Column)
              .str());

    return Token{TokenKind::String, Raw, Start};
  }
};

struct FieldDecl {
  std::string Name;
  uint64_t Width = 0;
};

struct RegisterDecl {
  std::string Name;
  uint64_t Width = 0;
  uint64_t Reset = 0;
};

struct InstructionDecl {
  std::string Name;
  std::vector<FieldDecl> Fields;
  std::string Encoding;
};

struct CoreDecl {
  std::string Name;
  std::vector<RegisterDecl> Registers;
  std::vector<InstructionDecl> Instructions;
};

class Parser {
public:
  explicit Parser(StringRef Buffer) : Lex(Buffer), Cur(Lex.next()) {}

  Expected<CoreDecl> parseCore() {
    if (!consume(TokenKind::KwCore, "expected 'core'"))
      return error();

    auto Name = parseIdentifier("expected core name");
    if (!Name)
      return error();

    if (!consume(TokenKind::LBrace, "expected '{' after core name"))
      return error();

    CoreDecl Core{*Name};
    while (Cur.Kind != TokenKind::RBrace && Cur.Kind != TokenKind::Eof) {
      if (Cur.Kind == TokenKind::KwRegister) {
        auto R = parseRegister();
        if (!R)
          return error();
        Core.Registers.push_back(std::move(*R));
        continue;
      }

      if (Cur.Kind == TokenKind::KwInstruction) {
        auto I = parseInstruction();
        if (!I)
          return error();
        Core.Instructions.push_back(std::move(*I));
        continue;
      }

      addDiag("expected 'register' or 'instruction'");
      return error();
    }

    if (!consume(TokenKind::RBrace, "expected '}' at end of core"))
      return error();

    if (Cur.Kind != TokenKind::Eof)
      addDiag("unexpected tokens after core declaration");

    if (!Diags.empty())
      return error();
    return Core;
  }

private:
  Lexer Lex;
  Token Cur;
  std::vector<std::string> Diags;

  void advance() { Cur = Lex.next(); }

  void addDiag(const Twine &Message) {
    Diags.push_back(formatv("{0}:{1}: {2}", Cur.Loc.Line, Cur.Loc.Column,
                            Message.str())
                        .str());
  }

  bool consume(TokenKind Kind, const Twine &Err) {
    if (Cur.Kind != Kind) {
      addDiag(Err);
      return false;
    }
    advance();
    return true;
  }

  std::optional<std::string> parseIdentifier(const Twine &Err) {
    if (Cur.Kind != TokenKind::Identifier) {
      addDiag(Err);
      return std::nullopt;
    }

    std::string Value = Cur.Lexeme.str();
    advance();
    return Value;
  }

  std::optional<uint64_t> parseNumber(const Twine &Err) {
    if (Cur.Kind != TokenKind::Number) {
      addDiag(Err);
      return std::nullopt;
    }

    uint64_t Value = 0;
    if (Cur.Lexeme.getAsInteger(0, Value)) {
      addDiag("invalid integer literal");
      return std::nullopt;
    }

    advance();
    return Value;
  }

  std::optional<RegisterDecl> parseRegister() {
    if (!consume(TokenKind::KwRegister, "expected 'register'"))
      return std::nullopt;

    auto Name = parseIdentifier("expected register name");
    if (!Name)
      return std::nullopt;

    if (!consume(TokenKind::LBrace, "expected '{' in register declaration"))
      return std::nullopt;

    RegisterDecl Reg;
    Reg.Name = *Name;

    while (Cur.Kind != TokenKind::RBrace && Cur.Kind != TokenKind::Eof) {
      if (Cur.Kind == TokenKind::KwWidth) {
        advance();
        if (!consume(TokenKind::Equal, "expected '=' after width"))
          return std::nullopt;
        auto Value = parseNumber("expected register width");
        if (!Value)
          return std::nullopt;
        Reg.Width = *Value;
        if (!consume(TokenKind::Semicolon, "expected ';' after width"))
          return std::nullopt;
        continue;
      }

      if (Cur.Kind == TokenKind::KwReset) {
        advance();
        if (!consume(TokenKind::Equal, "expected '=' after reset"))
          return std::nullopt;
        auto Value = parseNumber("expected register reset value");
        if (!Value)
          return std::nullopt;
        Reg.Reset = *Value;
        if (!consume(TokenKind::Semicolon, "expected ';' after reset"))
          return std::nullopt;
        continue;
      }

      addDiag("expected 'width' or 'reset' in register declaration");
      return std::nullopt;
    }

    if (!consume(TokenKind::RBrace, "expected '}' in register declaration"))
      return std::nullopt;
    if (!consume(TokenKind::Semicolon, "expected ';' after register declaration"))
      return std::nullopt;

    return Reg;
  }

  std::optional<InstructionDecl> parseInstruction() {
    if (!consume(TokenKind::KwInstruction, "expected 'instruction'"))
      return std::nullopt;

    auto Name = parseIdentifier("expected instruction name");
    if (!Name)
      return std::nullopt;

    if (!consume(TokenKind::LBrace, "expected '{' in instruction declaration"))
      return std::nullopt;

    InstructionDecl Inst;
    Inst.Name = *Name;

    while (Cur.Kind != TokenKind::RBrace && Cur.Kind != TokenKind::Eof) {
      if (Cur.Kind == TokenKind::KwField) {
        advance();
        auto FieldName = parseIdentifier("expected field name");
        if (!FieldName)
          return std::nullopt;
        if (!consume(TokenKind::LParen, "expected '(' after field name"))
          return std::nullopt;
        auto Width = parseNumber("expected field width");
        if (!Width)
          return std::nullopt;
        if (!consume(TokenKind::RParen, "expected ')' after field width"))
          return std::nullopt;
        if (!consume(TokenKind::Semicolon, "expected ';' after field declaration"))
          return std::nullopt;
        Inst.Fields.push_back(FieldDecl{*FieldName, *Width});
        continue;
      }

      if (Cur.Kind == TokenKind::KwEncoding) {
        advance();
        if (!consume(TokenKind::Equal, "expected '=' after encoding"))
          return std::nullopt;

        if (Cur.Kind != TokenKind::String) {
          addDiag("expected encoding string literal");
          return std::nullopt;
        }

        Inst.Encoding = Cur.Lexeme.str();
        advance();
        if (!consume(TokenKind::Semicolon, "expected ';' after encoding"))
          return std::nullopt;
        continue;
      }

      addDiag("expected 'field' or 'encoding' in instruction declaration");
      return std::nullopt;
    }

    if (!consume(TokenKind::RBrace, "expected '}' in instruction declaration"))
      return std::nullopt;
    if (!consume(TokenKind::Semicolon,
                 "expected ';' after instruction declaration"))
      return std::nullopt;

    return Inst;
  }

  Error error() {
    for (const std::string &Diag : Lex.Diags)
      Diags.push_back(Diag);

    std::string Joined;
    raw_string_ostream OS(Joined);
    for (StringRef Diag : Diags)
      OS << Diag << '\n';
    return createStringError(inconvertibleErrorCode(), OS.str());
  }
};

json::Value toJSON(const CoreDecl &Core) {
  json::Array Registers;
  for (const RegisterDecl &R : Core.Registers) {
    Registers.push_back(json::Object{{"name", R.Name},
                                     {"width", static_cast<int64_t>(R.Width)},
                                     {"reset", static_cast<int64_t>(R.Reset)}});
  }

  json::Array Instructions;
  for (const InstructionDecl &I : Core.Instructions) {
    json::Array Fields;
    for (const FieldDecl &F : I.Fields)
      Fields.push_back(json::Object{{"name", F.Name},
                                    {"width", static_cast<int64_t>(F.Width)}});

    Instructions.push_back(json::Object{{"name", I.Name},
                                        {"encoding", I.Encoding},
                                        {"fields", std::move(Fields)}});
  }

  return json::Object{{"core", Core.Name},
                      {"registers", std::move(Registers)},
                      {"instructions", std::move(Instructions)}};
}

} // namespace

static cl::OptionCategory TargetGenCategory("target-gen options");

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input CoreDSL file>"),
                                          cl::Required,
                                          cl::cat(TargetGenCategory));

static cl::opt<std::string>
    OutputFilename("o", cl::desc("Output filename"), cl::init("-"),
                   cl::value_desc("filename"), cl::cat(TargetGenCategory));

int main(int argc, const char **argv) {
  InitLLVM X(argc, argv);
  cl::HideUnrelatedOptions({&TargetGenCategory});
  cl::ParseCommandLineOptions(argc, argv, "CoreDSL frontend\n");

  ErrorOr<std::unique_ptr<MemoryBuffer>> BufferOrErr =
      MemoryBuffer::getFile(InputFilename);
  if (!BufferOrErr) {
    errs() << "target-gen: unable to read '" << InputFilename
           << "': " << BufferOrErr.getError().message() << '\n';
    return 1;
  }

  Parser P(BufferOrErr.get()->getBuffer());
  Expected<CoreDecl> CoreOrErr = P.parseCore();
  if (!CoreOrErr) {
    errs() << "target-gen: parse failed:\n"
           << toString(CoreOrErr.takeError());
    return 1;
  }

  std::error_code EC;
  raw_fd_ostream OS(OutputFilename, EC, sys::fs::OF_Text);
  if (EC) {
    errs() << "target-gen: unable to open output '" << OutputFilename
           << "': " << EC.message() << '\n';
    return 1;
  }

  OS << formatv("{0:2}\n", toJSON(*CoreOrErr));
  return 0;
}
