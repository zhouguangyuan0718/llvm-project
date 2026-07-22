//===-- Parser.cpp - CoreDSL syntax and semantic analysis ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Parser.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"
#include <cctype>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;
using namespace llvm::coredsl;

namespace {

enum class TokenKind { Identifier, Number, String, Punctuation, Eof };

struct Token {
  TokenKind Kind;
  StringRef Text;
  unsigned Line;
  unsigned Column;
};

class Lexer {
  StringRef Input;
  size_t Offset = 0;
  unsigned Line = 1;
  unsigned Column = 1;

  char peek(unsigned Ahead = 0) const {
    return Offset + Ahead < Input.size() ? Input[Offset + Ahead] : '\0';
  }

  char take() {
    char C = peek();
    if (C == '\n') {
      ++Line;
      Column = 1;
    } else if (C) {
      ++Column;
    }
    ++Offset;
    return C;
  }

  void skipTrivia() {
    for (;;) {
      while (std::isspace(static_cast<unsigned char>(peek())))
        take();
      if (peek() == '/' && peek(1) == '/') {
        while (peek() && peek() != '\n')
          take();
        continue;
      }
      if (peek() == '/' && peek(1) == '*') {
        take();
        take();
        while (peek() && !(peek() == '*' && peek(1) == '/'))
          take();
        if (peek()) {
          take();
          take();
        }
        continue;
      }
      return;
    }
  }

public:
  explicit Lexer(StringRef Input) : Input(Input) {}

  Token next() {
    skipTrivia();
    const size_t Start = Offset;
    const unsigned StartLine = Line, StartColumn = Column;
    if (!peek())
      return {TokenKind::Eof, "", StartLine, StartColumn};

    if (std::isalpha(static_cast<unsigned char>(peek())) || peek() == '_') {
      take();
      while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')
        take();
      return {TokenKind::Identifier, Input.slice(Start, Offset), StartLine,
              StartColumn};
    }
    if (std::isdigit(static_cast<unsigned char>(peek()))) {
      take();
      while (std::isalnum(static_cast<unsigned char>(peek())) ||
             peek() == '_' || peek() == '\'')
        take();
      return {TokenKind::Number, Input.slice(Start, Offset), StartLine,
              StartColumn};
    }
    if (peek() == '"') {
      take();
      while (peek() && peek() != '"') {
        if (peek() == '\\' && peek(1))
          take();
        take();
      }
      if (peek())
        take();
      return {TokenKind::String, Input.slice(Start, Offset), StartLine,
              StartColumn};
    }

    static constexpr StringRef Multi[] = {
        "[[", "]]", "::", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "+=",
        "-=", "*=", "/=", "%=", "&=", "|=", "^=", "++", "--", "+:", "-:"};
    for (StringRef P : Multi) {
      if (Input.drop_front(Offset).starts_with(P)) {
        for (size_t I = 0; I != P.size(); ++I)
          take();
        return {TokenKind::Punctuation, P, StartLine, StartColumn};
      }
    }
    take();
    return {TokenKind::Punctuation, Input.slice(Start, Offset), StartLine,
            StartColumn};
  }
};

class Parser {
  Lexer Lex;
  Token Cur;
  StringMap<std::string> Symbols;
  std::vector<StringMap<std::string>> Scopes;

  [[noreturn]] void fail(const Twine &Message) const {
    errs() << Cur.Line << ':' << Cur.Column << ": error: " << Message << '\n';
    exit(1);
  }

  void advance() { Cur = Lex.next(); }

  bool is(StringRef Text) const { return Cur.Text == Text; }

  bool consume(StringRef Text) {
    if (!is(Text))
      return false;
    advance();
    return true;
  }

  void expect(StringRef Text) {
    if (!consume(Text))
      fail(Twine("expected '") + Text + "'");
  }

  std::string expectIdentifier() {
    if (Cur.Kind != TokenKind::Identifier)
      fail("expected identifier");
    std::string Result = Cur.Text.str();
    advance();
    return Result;
  }

  SourceLoc loc() const { return {Cur.Line, Cur.Column}; }

  void enterScope() { Scopes.emplace_back(); }
  void leaveScope() { Scopes.pop_back(); }

  void define(StringRef Name, StringRef Type) {
    if (Scopes.empty())
      Symbols[Name] = Type.str();
    else
      Scopes.back()[Name] = Type.str();
  }

  bool knownSymbol(StringRef Name) const {
    if (Name == "X" || Name == "XW" || Name.starts_with("MEM"))
      return true;
    for (auto It = Scopes.rbegin(); It != Scopes.rend(); ++It)
      if (It->contains(Name))
        return true;
    return Symbols.contains(Name);
  }

  std::string parseBalanced(StringRef Open, StringRef Close) {
    expect(Open);
    unsigned Depth = 1;
    std::string Result;
    while (Depth) {
      if (Cur.Kind == TokenKind::Eof)
        fail("unexpected end of file in attribute");
      if (is(Open))
        ++Depth;
      if (is(Close) && --Depth == 0) {
        advance();
        break;
      }
      if (!Result.empty())
        Result += ' ';
      Result += Cur.Text.str();
      advance();
    }
    return Result;
  }

  static int precedence(StringRef Op) {
    if (Op == "=" || Op.ends_with("="))
      return 1;
    if (Op == "||")
      return 2;
    if (Op == "&&")
      return 3;
    if (Op == "|")
      return 4;
    if (Op == "^")
      return 5;
    if (Op == "&")
      return 6;
    if (Op == "==" || Op == "!=")
      return 7;
    if (Op == "<" || Op == ">" || Op == "<=" || Op == ">=")
      return 8;
    if (Op == "<<" || Op == ">>")
      return 9;
    if (Op == "+" || Op == "-")
      return 10;
    if (Op == "*" || Op == "/" || Op == "%")
      return 11;
    return -1;
  }

  std::unique_ptr<Expr> makeExpr(Expr::Kind K, SourceLoc L, StringRef Text) {
    auto Result = std::make_unique<Expr>();
    Result->K = K;
    Result->Loc = L;
    Result->Text = Text.str();
    return Result;
  }

  std::unique_ptr<Expr> parsePrimary() {
    SourceLoc L = loc();
    if (Cur.Kind == TokenKind::Number) {
      auto Result = makeExpr(Expr::Constant, L, Cur.Text);
      advance();
      return Result;
    }
    if (Cur.Kind == TokenKind::Identifier) {
      std::string Name = expectIdentifier();
      if (!knownSymbol(Name))
        fail(Twine("use of undefined symbol '") + Name + "'");
      auto Result = makeExpr(Expr::Symbol, L, Name);
      return parsePostfix(std::move(Result));
    }
    if (consume("(")) {
      if (is("signed") || is("unsigned")) {
        std::string Type = expectIdentifier();
        if (consume("<")) {
          Type += '<';
          while (!is(">")) {
            Type += Cur.Text.str();
            advance();
          }
          Type += '>';
          expect(">");
        }
        expect(")");
        auto Result = makeExpr(Expr::Cast, L, Type);
        Result->Args.push_back(parseUnary());
        return Result;
      }
      auto Result = parseExpression();
      expect(")");
      return parsePostfix(std::move(Result));
    }
    fail("expected expression");
  }

  std::unique_ptr<Expr> parsePostfix(std::unique_ptr<Expr> Base) {
    while (true) {
      SourceLoc L = loc();
      if (consume("[")) {
        auto Result = makeExpr(Expr::Subscript, L, "[]");
        Result->Args.push_back(std::move(Base));
        Result->Args.push_back(parseExpression());
        if (consume(":"))
          Result->Args.push_back(parseExpression());
        else if (consume("+:"))
          Result->Args.push_back(parseExpression());
        else if (consume("-:"))
          Result->Args.push_back(parseExpression());
        expect("]");
        Base = std::move(Result);
      } else if (consume("(")) {
        auto Result = makeExpr(Expr::Call, L, "call");
        Result->Args.push_back(std::move(Base));
        if (!is(")")) {
          do {
            Result->Args.push_back(parseExpression());
          } while (consume(","));
        }
        expect(")");
        Base = std::move(Result);
      } else if (is("++") || is("--")) {
        auto Result = makeExpr(Expr::Unary, L, Cur.Text);
        Result->Args.push_back(std::move(Base));
        advance();
        Base = std::move(Result);
      } else {
        return Base;
      }
    }
  }

  std::unique_ptr<Expr> parseUnary() {
    if (is("-") || is("+") || is("!") || is("~") || is("++") || is("--")) {
      SourceLoc L = loc();
      std::string Op = Cur.Text.str();
      advance();
      auto Result = makeExpr(Expr::Unary, L, Op);
      Result->Args.push_back(parseUnary());
      return Result;
    }
    return parsePrimary();
  }

  std::unique_ptr<Expr> parseExpression(int MinPrec = 1) {
    auto Left = parseUnary();
    while (true) {
      if (is("?") && MinPrec <= 1) {
        SourceLoc L = loc();
        advance();
        auto Result = makeExpr(Expr::Ternary, L, "?:");
        Result->Args.push_back(std::move(Left));
        Result->Args.push_back(parseExpression());
        expect(":");
        Result->Args.push_back(parseExpression(1));
        Left = std::move(Result);
        continue;
      }
      int Prec = precedence(Cur.Text);
      if (Prec < MinPrec)
        return Left;
      SourceLoc L = loc();
      std::string Op = Cur.Text.str();
      advance();
      auto Right =
          parseExpression(Prec + (StringRef(Op).ends_with("=") ? 0 : 1));
      auto Result = makeExpr(Expr::Binary, L, Op);
      Result->Args.push_back(std::move(Left));
      Result->Args.push_back(std::move(Right));
      Left = std::move(Result);
    }
  }

  std::string parseType() {
    if (!is("signed") && !is("unsigned"))
      fail("expected signed or unsigned type");
    std::string Type = expectIdentifier();
    if (consume("<")) {
      Type += '<';
      unsigned Depth = 1;
      while (Depth) {
        if (Cur.Kind == TokenKind::Eof)
          fail("unexpected end of file in type");
        if (is("<"))
          ++Depth;
        if (is(">") && --Depth == 0) {
          Type += '>';
          advance();
          break;
        }
        Type += Cur.Text.str();
        advance();
      }
    }
    return Type;
  }

  std::unique_ptr<Stmt> parseDeclaration(bool ConsumeSemicolon = true) {
    auto Result = std::make_unique<Stmt>();
    Result->K = Stmt::Declaration;
    Result->Loc = loc();
    Result->Type = parseType();
    Result->Name = expectIdentifier();
    define(Result->Name, Result->Type);
    if (consume("="))
      Result->Value = parseExpression();
    if (ConsumeSemicolon)
      expect(";");
    return Result;
  }

  std::unique_ptr<Stmt> parseStatement() {
    SourceLoc L = loc();
    if (consume("{")) {
      auto Result = std::make_unique<Stmt>();
      Result->K = Stmt::Block;
      Result->Loc = L;
      enterScope();
      while (!consume("}")) {
        if (Cur.Kind == TokenKind::Eof)
          fail("unexpected end of file in behaviour block");
        Result->Children.push_back(parseStatement());
      }
      leaveScope();
      return Result;
    }
    if (consume("if")) {
      auto Result = std::make_unique<Stmt>();
      Result->K = Stmt::If;
      Result->Loc = L;
      expect("(");
      Result->Condition = parseExpression();
      expect(")");
      Result->Children.push_back(parseStatement());
      if (consume("else"))
        Result->Children.push_back(parseStatement());
      return Result;
    }
    if (consume("while")) {
      auto Result = std::make_unique<Stmt>();
      Result->K = Stmt::While;
      Result->Loc = L;
      expect("(");
      Result->Condition = parseExpression();
      expect(")");
      Result->Children.push_back(parseStatement());
      return Result;
    }
    if (consume("for")) {
      auto Result = std::make_unique<Stmt>();
      Result->K = Stmt::For;
      Result->Loc = L;
      expect("(");
      enterScope();
      if (!is(";")) {
        if (is("signed") || is("unsigned"))
          Result->Children.push_back(parseDeclaration(false));
        else {
          auto Init = std::make_unique<Stmt>();
          Init->K = Stmt::Expression;
          Init->Loc = loc();
          Init->Value = parseExpression();
          Result->Children.push_back(std::move(Init));
        }
      }
      expect(";");
      if (!is(";"))
        Result->Condition = parseExpression();
      expect(";");
      if (!is(")"))
        Result->Step = parseExpression();
      expect(")");
      Result->Children.push_back(parseStatement());
      leaveScope();
      return Result;
    }
    if (is("signed") || is("unsigned"))
      return parseDeclaration();

    auto Result = std::make_unique<Stmt>();
    Result->K = Stmt::Expression;
    Result->Loc = L;
    Result->Value = parseExpression();
    expect(";");
    return Result;
  }

  void parseOperands(Instruction &Inst) {
    expect("operands");
    expect(":");
    const bool HasBraces = consume("{");
    while (!(HasBraces && is("}"))) {
      if (!is("signed") && !is("unsigned"))
        break;
      Operand Op;
      Op.Loc = loc();
      Op.Type = parseType();
      Op.Name = expectIdentifier();
      if (is("[["))
        Op.Attributes = parseBalanced("[[", "]]");
      expect(";");
      define(Op.Name, Op.Type);
      Inst.Operands.push_back(std::move(Op));
    }
    if (HasBraces)
      expect("}");
  }

  static unsigned literalWidth(StringRef Text) {
    size_t Quote = Text.find('\'');
    if (Quote == StringRef::npos)
      return 0;
    unsigned Width = 0;
    if (Text.take_front(Quote).getAsInteger(10, Width))
      return 0;
    return Width;
  }

  void parseEncoding(Instruction &Inst) {
    expect("encoding");
    expect(":");
    unsigned TotalWidth = 0;
    while (true) {
      EncodingFragment Fragment;
      Fragment.Loc = loc();
      if (Cur.Kind == TokenKind::Number) {
        Fragment.IsConstant = true;
        Fragment.Text = Cur.Text.str();
        Fragment.Width = literalWidth(Cur.Text);
        if (!Fragment.Width)
          fail("encoding constants require a sized literal such as 7'b0110011");
        advance();
      } else if (Cur.Kind == TokenKind::Identifier) {
        Fragment.Text = expectIdentifier();
        expect("[");
        StringRef Start = Cur.Text;
        if (Cur.Kind != TokenKind::Number)
          fail("expected encoding bit index");
        unsigned High = 0;
        if (Start.getAsInteger(10, High))
          fail("invalid encoding bit index");
        advance();
        unsigned Low = High;
        if (consume(":")) {
          if (Cur.Kind != TokenKind::Number || Cur.Text.getAsInteger(10, Low))
            fail("invalid encoding bit index");
          advance();
        }
        if (Low > High)
          fail("encoding range has low bit greater than high bit");
        Fragment.Width = High - Low + 1;
        Fragment.Text +=
            "[" + std::to_string(High) + ":" + std::to_string(Low) + "]";
        StringRef FieldName = StringRef(Fragment.Text).split('[').first;
        if (!Symbols.contains(FieldName))
          Symbols[FieldName] = formatv("bits<{0}>", Fragment.Width).str();
        expect("]");
      } else {
        fail("expected encoding fragment");
      }
      TotalWidth += Fragment.Width;
      Inst.Encoding.push_back(std::move(Fragment));
      if (consume(";"))
        break;
      expect("::");
    }
    if (!TotalWidth || TotalWidth > 64)
      fail("instruction encoding width must be between 1 and 64 bits");
  }

  void parseAssembly(Instruction &Inst) {
    expect("assembly");
    expect(":");
    if (Cur.Kind == TokenKind::String) {
      Inst.Assembly = Cur.Text.str();
      advance();
    } else if (consume("{")) {
      while (!is("}")) {
        if (!Inst.Assembly.empty())
          Inst.Assembly += ' ';
        Inst.Assembly += Cur.Text.str();
        advance();
      }
      expect("}");
    } else {
      fail("expected assembly string");
    }
    expect(";");
  }

  Instruction parseInstruction() {
    Instruction Inst;
    Inst.Loc = loc();
    Inst.Name = expectIdentifier();
    while (is("[["))
      parseBalanced("[[", "]]");
    expect("{");
    Symbols.clear();
    Symbols["XLEN"] = "parameter";
    Symbols["RFS"] = "parameter";
    while (!consume("}")) {
      if (Cur.Kind == TokenKind::Eof)
        fail("unexpected end of file in instruction");
      if (is("operands")) {
        parseOperands(Inst);
      } else if (is("encoding")) {
        parseEncoding(Inst);
      } else if (is("assembly")) {
        parseAssembly(Inst);
      } else if (consume("behavior")) {
        expect(":");
        Inst.Behaviour = parseStatement();
      } else {
        fail("expected operands, encoding, assembly, or behavior section");
      }
    }
    if (Inst.Encoding.empty())
      fail("instruction is missing encoding");
    if (!Inst.Behaviour)
      fail("instruction is missing behavior");
    return Inst;
  }

public:
  explicit Parser(StringRef Input) : Lex(Input) { advance(); }

  Module parseModule() {
    Module Result;
    if (consume("InstructionSet")) {
      Result.Name = expectIdentifier();
      if (consume("extends"))
        expectIdentifier();
      while (is("[["))
        parseBalanced("[[", "]]");
      expect("{");
      expect("instructions");
      expect("{");
      while (!consume("}"))
        Result.Instructions.push_back(parseInstruction());
      expect("}");
    } else {
      while (Cur.Kind != TokenKind::Eof)
        Result.Instructions.push_back(parseInstruction());
    }
    return Result;
  }
};

} // namespace

Module llvm::coredsl::parseCoreDSL(StringRef Input) {
  return Parser(Input).parseModule();
}
