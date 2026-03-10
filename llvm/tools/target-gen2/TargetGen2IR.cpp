//===- TargetGen2IR.cpp - CoreDSL LLVM IR emission -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TargetGen2IR.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include <cctype>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace llvm;
using namespace llvm::targetgen2;

namespace {

static bool parseIntegerLiteral(StringRef S, uint64_t &Out) {
  StringRef T = S.trim();
  if (T.empty() || T.contains("'"))
    return false;
  unsigned Base = 10;
  if (T.starts_with("0x") || T.starts_with("0X")) {
    Base = 16;
    T = T.drop_front(2);
  }
  if (T.empty())
    return false;
  Out = 0;
  for (char C : T) {
    if (!std::isxdigit(static_cast<unsigned char>(C)))
      return false;
    unsigned V = std::isdigit(static_cast<unsigned char>(C))
                     ? static_cast<unsigned>(C - '0')
                     : static_cast<unsigned>(std::tolower(C) - 'a' + 10);
    if (V >= Base)
      return false;
    Out = Out * Base + V;
  }
  return true;
}

static std::optional<unsigned> parseWidth(StringRef Tok) {
  Tok = Tok.trim();
  if (Tok.empty())
    return std::nullopt;
  if (Tok == "XLEN")
    return 64;
  unsigned W = 0;
  for (char C : Tok) {
    if (!std::isdigit(static_cast<unsigned char>(C)))
      return std::nullopt;
    W = W * 10 + static_cast<unsigned>(C - '0');
  }
  return W;
}

static bool isMemIndex(const Expr &E) {
  return E.Kind == ExprKind::Index && E.Children.size() >= 2 &&
         E.Children[0].Kind == ExprKind::Identifier && E.Children[0].Text == "MEM";
}

struct ModuleState {
  LLVMContext Ctx;
  std::unique_ptr<Module> M;
  Function *MemLoadDecl = nullptr;
  Function *MemStoreDecl = nullptr;

  ModuleState() : M(std::make_unique<Module>("target-gen2", Ctx)) {
    M->setSourceFileName("target-gen2");
  }

  Type *i64() { return Type::getInt64Ty(Ctx); }
  Type *i1() { return Type::getInt1Ty(Ctx); }

  Function *getMemLoad() {
    if (MemLoadDecl)
      return MemLoadDecl;
    FunctionType *FT = FunctionType::get(i64(), {i64()}, false);
    MemLoadDecl = Function::Create(FT, Function::ExternalLinkage, "tg2.mem.load", *M);
    return MemLoadDecl;
  }

  Function *getMemStore() {
    if (MemStoreDecl)
      return MemStoreDecl;
    FunctionType *FT = FunctionType::get(Type::getVoidTy(Ctx), {i64(), i64()}, false);
    MemStoreDecl = Function::Create(FT, Function::ExternalLinkage, "tg2.mem.store", *M);
    return MemStoreDecl;
  }
};

struct FunctionEmitter {
  ModuleState &MS;
  Function &F;
  IRBuilder<> B;
  std::unordered_map<std::string, Value *> Env;
  bool Terminated = false;
  Value *LastValue = nullptr;

  explicit FunctionEmitter(ModuleState &MS, Function &F)
      : MS(MS), F(F), B(MS.Ctx) {
    BasicBlock *Entry = BasicBlock::Create(MS.Ctx, "entry", &F);
    B.SetInsertPoint(Entry);
    for (Argument &Arg : F.args())
      Env[std::string(Arg.getName())] = &Arg;
    LastValue = ConstantInt::get(MS.i64(), 0);
  }

  Value *toI64(Value *V) {
    if (!V)
      return ConstantInt::get(MS.i64(), 0);
    if (V->getType()->isIntegerTy(64))
      return V;
    if (V->getType()->isIntegerTy(1))
      return B.CreateZExt(V, MS.i64(), "zext.i1.i64");
    if (V->getType()->isIntegerTy())
      return B.CreateSExtOrTrunc(V, MS.i64(), "cast.i64");
    return ConstantInt::get(MS.i64(), 0);
  }

  Value *toI1(Value *V) {
    if (!V)
      return ConstantInt::getFalse(MS.Ctx);
    if (V->getType()->isIntegerTy(1))
      return V;
    Value *I64 = toI64(V);
    return B.CreateICmpNE(I64, ConstantInt::get(MS.i64(), 0), "cond");
  }

  Value *atomFromText(StringRef Tok) {
    Tok = Tok.trim();
    auto It = Env.find(Tok.str());
    if (It != Env.end())
      return It->second;
    uint64_t Lit = 0;
    if (parseIntegerLiteral(Tok, Lit))
      return ConstantInt::get(MS.i64(), Lit);
    return ConstantInt::get(MS.i64(), 0);
  }

  Value *emitTextExpr(StringRef Text) {
    static constexpr StringLiteral Ops[] = {"!=", "==", "<=", ">=", "<", ">",
                                             "+",  "-",  "*",  "/"};
    for (StringLiteral Op : Ops) {
      size_t Pos = Text.find(Op);
      if (Pos == StringRef::npos)
        continue;
      Value *L = toI64(atomFromText(Text.take_front(Pos)));
      Value *R = toI64(atomFromText(Text.drop_front(Pos + Op.size())));
      if (Op == "!=")
        return B.CreateICmpNE(L, R, "ne");
      if (Op == "==")
        return B.CreateICmpEQ(L, R, "eq");
      if (Op == "<=")
        return B.CreateICmpSLE(L, R, "le");
      if (Op == ">=")
        return B.CreateICmpSGE(L, R, "ge");
      if (Op == "<")
        return B.CreateICmpSLT(L, R, "lt");
      if (Op == ">")
        return B.CreateICmpSGT(L, R, "gt");
      if (Op == "+")
        return B.CreateAdd(L, R, "add");
      if (Op == "-")
        return B.CreateSub(L, R, "sub");
      if (Op == "*")
        return B.CreateMul(L, R, "mul");
      if (Op == "/")
        return B.CreateSDiv(L, R, "div");
    }
    return atomFromText(Text);
  }

  Value *emitExpr(const Expr &E) {
    switch (E.Kind) {
    case ExprKind::Identifier: {
      auto It = Env.find(E.Text);
      if (It != Env.end())
        return It->second;
      return ConstantInt::get(MS.i64(), 0);
    }
    case ExprKind::Literal: {
      uint64_t Lit = 0;
      if (parseIntegerLiteral(E.Value.empty() ? E.Text : E.Value, Lit))
        return ConstantInt::get(MS.i64(), Lit);
      return ConstantInt::get(MS.i64(), 0);
    }
    case ExprKind::Group:
      if (!E.Children.empty())
        return emitExpr(E.Children.back());
      return ConstantInt::get(MS.i64(), 0);
    case ExprKind::Cast: {
      StringRef TypeText = E.Value.empty() ? E.Text : E.Value;
      if (E.Children.empty())
        return emitTextExpr(TypeText);
      Value *V = toI64(emitExpr(E.Children.back()));
      SmallVector<StringRef, 4> Parts;
      TypeText.split(Parts, ' ', -1, false);
      bool IsUnsigned = !Parts.empty() && Parts.front() == "unsigned";
      StringRef WidthTok;
      if (IsUnsigned && Parts.size() >= 2)
        WidthTok = Parts[1];
      else if (!Parts.empty())
        WidthTok = Parts.back();
      std::optional<unsigned> Width = parseWidth(WidthTok);
      if (!IsUnsigned) {
        if (E.Children.back().Kind == ExprKind::Unknown)
          return emitTextExpr(TypeText);
        return V;
      }
      if (!Width || *Width >= 64)
        return V;
      uint64_t Mask = (1ULL << *Width) - 1ULL;
      return B.CreateAnd(V, ConstantInt::get(MS.i64(), Mask), "unsigned.mask");
    }
    case ExprKind::Binary: {
      if (E.Children.size() < 2)
        return ConstantInt::get(MS.i64(), 0);
      Value *L = toI64(emitExpr(E.Children[0]));
      Value *R = toI64(emitExpr(E.Children[1]));
      switch (E.Op) {
      case ExprOp::Add:
        return B.CreateAdd(L, R, "add");
      case ExprOp::Sub:
        return B.CreateSub(L, R, "sub");
      case ExprOp::Mul:
        return B.CreateMul(L, R, "mul");
      case ExprOp::Div:
        return B.CreateSDiv(L, R, "div");
      case ExprOp::Eq:
        return B.CreateICmpEQ(L, R, "eq");
      case ExprOp::Ne:
        return B.CreateICmpNE(L, R, "ne");
      case ExprOp::Lt:
        return B.CreateICmpSLT(L, R, "lt");
      case ExprOp::Le:
        return B.CreateICmpSLE(L, R, "le");
      case ExprOp::Gt:
        return B.CreateICmpSGT(L, R, "gt");
      case ExprOp::Ge:
        return B.CreateICmpSGE(L, R, "ge");
      default:
        return ConstantInt::get(MS.i64(), 0);
      }
    }
    case ExprKind::Index:
      if (isMemIndex(E)) {
        Value *Addr = toI64(emitExpr(E.Children[1]));
        return B.CreateCall(MS.getMemLoad(), {Addr}, "mem.load");
      }
      return ConstantInt::get(MS.i64(), 0);
    case ExprKind::Assignment: {
      if (E.Children.size() < 2)
        return ConstantInt::get(MS.i64(), 0);
      const Expr &LHS = E.Children[0];
      Value *RHS = toI64(emitExpr(E.Children[1]));
      if (LHS.Kind == ExprKind::Identifier)
        Env[LHS.Text] = RHS;
      else if (isMemIndex(LHS)) {
        Value *Addr = toI64(emitExpr(LHS.Children[1]));
        B.CreateCall(MS.getMemStore(), {Addr, RHS});
      }
      LastValue = RHS;
      return RHS;
    }
    default:
      if (!E.Text.empty() || !E.Value.empty())
        return emitTextExpr(E.Value.empty() ? E.Text : E.Value);
      return ConstantInt::get(MS.i64(), 0);
    }
  }

  void emitStmt(const Statement &S) {
    if (Terminated)
      return;
    switch (S.Kind) {
    case StatementKind::Compound:
      for (const Statement &C : S.Children)
        emitStmt(C);
      return;
    case StatementKind::Expression:
      for (const Expr &E : S.Expressions)
        LastValue = toI64(emitExpr(E));
      return;
    case StatementKind::Return: {
      Value *RV = ConstantInt::get(MS.i64(), 0);
      if (!S.Expressions.empty())
        RV = toI64(emitExpr(S.Expressions.front()));
      B.CreateRet(RV);
      Terminated = true;
      return;
    }
    case StatementKind::If: {
      if (S.Expressions.empty() || S.Children.empty())
        return;
      Value *Cond = toI1(emitExpr(S.Expressions.front()));
      BasicBlock *ThenBB = BasicBlock::Create(MS.Ctx, "if.then", &F);
      BasicBlock *ElseBB = BasicBlock::Create(MS.Ctx, "if.else", &F);
      BasicBlock *MergeBB = BasicBlock::Create(MS.Ctx, "if.end", &F);
      B.CreateCondBr(Cond, ThenBB, ElseBB);

      B.SetInsertPoint(ThenBB);
      Terminated = false;
      emitStmt(S.Children[0]);
      bool ThenTerminated = Terminated;
      if (!ThenTerminated)
        B.CreateBr(MergeBB);

      B.SetInsertPoint(ElseBB);
      Terminated = false;
      if (S.Children.size() > 1)
        emitStmt(S.Children[1]);
      bool ElseTerminated = Terminated;
      if (!ElseTerminated)
        B.CreateBr(MergeBB);

      if (ThenTerminated && ElseTerminated) {
        Terminated = true;
        return;
      }
      B.SetInsertPoint(MergeBB);
      Terminated = false;
      return;
    }
    default:
      return;
    }
  }
};

static std::vector<std::string>
inferParams(const llvm::targetgen2::Instruction &Inst) {
  std::vector<std::string> Params;
  std::unordered_set<std::string> Seen;
  for (const EncodingField &F : Inst.Encoding) {
    if (F.IsBitValue || F.Name.empty() || F.Name == "MEM")
      continue;
    if (Seen.insert(F.Name).second)
      Params.push_back(F.Name);
  }
  return Params;
}

static void emitInstructionFunction(ModuleState &MS, StringRef Scope,
                                    const llvm::targetgen2::Instruction &Inst) {
  std::vector<std::string> Params = inferParams(Inst);
  SmallVector<Type *, 8> ParamTypes;
  ParamTypes.assign(Params.size(), MS.i64());
  FunctionType *FT = FunctionType::get(MS.i64(), ParamTypes, false);
  std::string Name = ("tg2.exec." + Scope + "." + Inst.Name).str();
  Function *F = Function::Create(FT, Function::ExternalLinkage, Name, *MS.M);

  size_t Idx = 0;
  for (Argument &Arg : F->args())
    Arg.setName(Params[Idx++]);

  FunctionEmitter E(MS, *F);
  E.emitStmt(Inst.Behavior);
  if (!E.Terminated)
    E.B.CreateRet(E.toI64(E.LastValue));
}

} // namespace

std::string llvm::targetgen2::toLLVMIR(const Description &Desc) {
  ModuleState MS;

  for (const InstructionSetDef &IS : Desc.InstructionSets) {
    std::string Scope = ("inst." + IS.Name);
    for (const llvm::targetgen2::Instruction &Inst : IS.ISA.Instructions)
      emitInstructionFunction(MS, Scope, Inst);
  }

  for (const CoreDef &C : Desc.Cores) {
    std::string Scope = ("core." + C.Name);
    for (const llvm::targetgen2::Instruction &Inst : C.ISA.Instructions)
      emitInstructionFunction(MS, Scope, Inst);
  }

  std::string Out;
  raw_string_ostream OS(Out);
  MS.M->print(OS, nullptr);
  return Out;
}
