//===- TargetGen2IR.cpp - CoreDSL LLVM IR emission --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TargetGen2IR.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace llvm;
using namespace llvm::targetgen2;

namespace {

std::string sanitizeName(StringRef S) {
  std::string Out;
  Out.reserve(S.size());
  for (char C : S) {
    if (isAlnum(static_cast<unsigned char>(C)) || C == '_' || C == '.')
      Out.push_back(C);
    else
      Out.push_back('_');
  }
  if (Out.empty())
    Out = "anon";
  return Out;
}

GlobalVariable *addCStringGlobal(Module &M, StringRef Name, StringRef Value) {
  LLVMContext &Ctx = M.getContext();
  Constant *Init = ConstantDataArray::getString(Ctx, Value, true);
  auto *GV = new GlobalVariable(M, Init->getType(), true,
                                GlobalValue::PrivateLinkage, Init, Name);
  GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
  GV->setAlignment(Align(1));
  return GV;
}

class FunctionEmitter {
public:
  FunctionEmitter(LLVMContext &Ctx, Function &F)
      : Ctx(Ctx), Builder(Ctx), F(F), I64(Type::getInt64Ty(Ctx)) {
    Entry = BasicBlock::Create(Ctx, "entry", &F);
    Builder.SetInsertPoint(Entry);
  }

  void setParam(StringRef Name, Value *V) { Vars[Name] = V; }

  void emitBehavior(const Statement &S) {
    emitStmt(S);
    if (!Builder.GetInsertBlock()->getTerminator())
      Builder.CreateRet(ConstantInt::get(I64, 0));
  }

private:
  using Env = StringMap<Value *>;

  LLVMContext &Ctx;
  IRBuilder<> Builder;
  Function &F;
  BasicBlock *Entry = nullptr;
  Type *I64 = nullptr;
  Env Vars;

  APInt parseInt(StringRef T) {
    T = T.trim();
    APInt V(64, 0);
    if (T.starts_with("0x"))
      return APInt(64, T.drop_front(2), 16);
    if (!T.getAsInteger(10, V))
      return V;
    return APInt(64, 0);
  }

  Value *toBool(Value *V) {
    return Builder.CreateICmpNE(V, ConstantInt::get(I64, 0));
  }

  static std::optional<ExprOp> textOp(StringRef T, size_t &Pos, unsigned &Len) {
    struct OpInfo {
      StringRef Op;
      ExprOp Kind;
    };
    static constexpr OpInfo Ops[] = {{"!=", ExprOp::Ne}, {"==", ExprOp::Eq},
                                     {"<=", ExprOp::Le}, {">=", ExprOp::Ge},
                                     {"<<", ExprOp::Shl}, {">>", ExprOp::Shr},
                                     {"<", ExprOp::Lt},  {">", ExprOp::Gt},
                                     {"+", ExprOp::Add}, {"-", ExprOp::Sub},
                                     {"*", ExprOp::Mul}, {"/", ExprOp::Div},
                                     {"&", ExprOp::BitAnd}, {"|", ExprOp::BitOr},
                                     {"^", ExprOp::BitXor}};
    for (const auto &OI : Ops) {
      Pos = T.find(OI.Op);
      if (Pos != StringRef::npos) {
        Len = OI.Op.size();
        return OI.Kind;
      }
    }
    return std::nullopt;
  }

  Value *lookupVar(StringRef Name) {
    auto It = Vars.find(Name);
    if (It != Vars.end())
      return It->second;
    return ConstantInt::get(I64, 0);
  }

  Value *emitBinary(Value *L, Value *R, ExprOp Op) {
    switch (Op) {
    case ExprOp::Add:
      return Builder.CreateAdd(L, R);
    case ExprOp::Sub:
      return Builder.CreateSub(L, R);
    case ExprOp::Mul:
      return Builder.CreateMul(L, R);
    case ExprOp::Div:
      return Builder.CreateSDiv(L, R);
    case ExprOp::Mod:
      return Builder.CreateSRem(L, R);
    case ExprOp::Eq:
      return Builder.CreateZExt(Builder.CreateICmpEQ(L, R), I64);
    case ExprOp::Ne:
      return Builder.CreateZExt(Builder.CreateICmpNE(L, R), I64);
    case ExprOp::Lt:
      return Builder.CreateZExt(Builder.CreateICmpSLT(L, R), I64);
    case ExprOp::Le:
      return Builder.CreateZExt(Builder.CreateICmpSLE(L, R), I64);
    case ExprOp::Gt:
      return Builder.CreateZExt(Builder.CreateICmpSGT(L, R), I64);
    case ExprOp::Ge:
      return Builder.CreateZExt(Builder.CreateICmpSGE(L, R), I64);
    case ExprOp::BitAnd:
      return Builder.CreateAnd(L, R);
    case ExprOp::BitOr:
      return Builder.CreateOr(L, R);
    case ExprOp::BitXor:
      return Builder.CreateXor(L, R);
    case ExprOp::Shl:
      return Builder.CreateShl(L, R);
    case ExprOp::Shr:
      return Builder.CreateAShr(L, R);
    default:
      return L;
    }
  }

  std::optional<Value *> emitSimpleTextExpr(StringRef T) {
    T = T.trim();
    size_t Pos = StringRef::npos;
    unsigned Len = 0;
    auto Op = textOp(T, Pos, Len);
    if (!Op)
      return std::nullopt;

    StringRef LHS = T.take_front(Pos).trim();
    StringRef RHS = T.drop_front(Pos + Len).trim();

    auto parseValue = [&](StringRef S) -> Value * {
      APInt Parsed(64, 0);
      if (!S.getAsInteger(10, Parsed))
        return ConstantInt::get(I64, Parsed);
      return lookupVar(S);
    };

    return emitBinary(parseValue(LHS), parseValue(RHS), *Op);
  }

  Value *emitExpr(const Expr &E) {
    switch (E.Kind) {
    case ExprKind::Literal:
      return ConstantInt::get(I64, parseInt(E.Value.empty() ? E.Text : E.Value));
    case ExprKind::Identifier:
      return lookupVar(E.Value.empty() ? E.Text : E.Value);
    case ExprKind::Assignment: {
      if (E.Children.size() < 2)
        return ConstantInt::get(I64, 0);
      const Expr &LHS = E.Children[0];
      Value *R = emitExpr(E.Children[1]);
      if (LHS.Kind == ExprKind::Identifier)
        Vars[LHS.Value.empty() ? LHS.Text : LHS.Value] = R;
      return R;
    }
    case ExprKind::Binary:
      if (E.Children.size() < 2)
        return ConstantInt::get(I64, 0);
      return emitBinary(emitExpr(E.Children[0]), emitExpr(E.Children[1]), E.Op);
    case ExprKind::Unary: {
      if (E.Children.empty())
        return ConstantInt::get(I64, 0);
      Value *V = emitExpr(E.Children[0]);
      switch (E.Op) {
      case ExprOp::UnaryMinus:
        return Builder.CreateNeg(V);
      case ExprOp::LogicalNot:
        return Builder.CreateZExt(Builder.CreateICmpEQ(V, ConstantInt::get(I64, 0)),
                                  I64);
      case ExprOp::BitNot:
        return Builder.CreateNot(V);
      default:
        return V;
      }
    }
    case ExprKind::Group:
    case ExprKind::Cast:
      if (!E.Children.empty()) {
        Value *V = emitExpr(E.Children[0]);
        if (E.Children[0].Kind != ExprKind::Unknown)
          return V;
      }
      if (!E.Value.empty()) {
        if (auto V = emitSimpleTextExpr(E.Value))
          return *V;
      }
      return ConstantInt::get(I64, 0);
    default:
      if (!E.Children.empty())
        return emitExpr(E.Children.back());
      return ConstantInt::get(I64, 0);
    }
  }

  void mergeEnvs(const Env &ThenEnv, BasicBlock *ThenBB, const Env &ElseEnv,
                 BasicBlock *ElseBB, BasicBlock *MergeBB) {
    std::set<std::string> Keys;
    for (const auto &KV : ThenEnv)
      Keys.insert(std::string(KV.first()));
    for (const auto &KV : ElseEnv)
      Keys.insert(std::string(KV.first()));

    for (const std::string &K : Keys) {
      Value *TV = nullptr;
      if (auto It = ThenEnv.find(K); It != ThenEnv.end())
        TV = It->second;
      Value *EV = nullptr;
      if (auto It = ElseEnv.find(K); It != ElseEnv.end())
        EV = It->second;
      if (!TV)
        TV = ConstantInt::get(I64, 0);
      if (!EV)
        EV = ConstantInt::get(I64, 0);
      if (TV == EV) {
        Vars[K] = TV;
        continue;
      }
      PHINode *PN = Builder.CreatePHI(I64, 2, sanitizeName(K) + ".phi");
      PN->addIncoming(TV, ThenBB);
      PN->addIncoming(EV, ElseBB);
      Vars[K] = PN;
    }
  }

  void emitStmt(const Statement &S) {
    if (Builder.GetInsertBlock()->getTerminator())
      return;

    switch (S.Kind) {
    case StatementKind::Compound:
      for (const Statement &C : S.Children)
        emitStmt(C);
      return;
    case StatementKind::Expression:
      for (const Expr &E : S.Expressions)
        (void)emitExpr(E);
      return;
    case StatementKind::If: {
      Value *CondV = ConstantInt::get(I64, 1);
      if (!S.Expressions.empty())
        CondV = emitExpr(S.Expressions.front());
      Value *Cond = toBool(CondV);

      BasicBlock *ThenBB = BasicBlock::Create(Ctx, "if.then", &F);
      BasicBlock *ElseBB = BasicBlock::Create(Ctx, "if.else", &F);
      BasicBlock *MergeBB = BasicBlock::Create(Ctx, "if.end", &F);
      Builder.CreateCondBr(Cond, ThenBB, ElseBB);

      Env Before = Vars;

      Builder.SetInsertPoint(ThenBB);
      Vars = Before;
      if (!S.Children.empty())
        emitStmt(S.Children[0]);
      if (!Builder.GetInsertBlock()->getTerminator())
        Builder.CreateBr(MergeBB);
      BasicBlock *ThenEnd = Builder.GetInsertBlock();
      Env ThenEnv = Vars;

      Builder.SetInsertPoint(ElseBB);
      Vars = Before;
      if (S.Children.size() > 1)
        emitStmt(S.Children[1]);
      if (!Builder.GetInsertBlock()->getTerminator())
        Builder.CreateBr(MergeBB);
      BasicBlock *ElseEnd = Builder.GetInsertBlock();
      Env ElseEnv = Vars;

      Builder.SetInsertPoint(MergeBB);
      mergeEnvs(ThenEnv, ThenEnd, ElseEnv, ElseEnd, MergeBB);
      return;
    }
    case StatementKind::Return: {
      Value *Ret = ConstantInt::get(I64, 0);
      if (!S.Expressions.empty())
        Ret = emitExpr(S.Expressions.front());
      Builder.CreateRet(Ret);
      return;
    }
    case StatementKind::Empty:
      return;
    default:
      for (const Expr &E : S.Expressions)
        (void)emitExpr(E);
      for (const Statement &C : S.Children)
        emitStmt(C);
      return;
    }
  }
};

std::vector<std::string> collectParams(const llvm::targetgen2::Instruction &Inst) {
  std::vector<std::string> Params;
  std::set<std::string> Seen;
  for (const EncodingField &EF : Inst.Encoding) {
    if (EF.IsBitValue || EF.Name.empty())
      continue;
    std::string N = sanitizeName(EF.Name);
    if (Seen.insert(N).second)
      Params.push_back(N);
  }
  return Params;
}

void emitInstructionBehavior(Module &M, StringRef Prefix, const llvm::targetgen2::Instruction &Inst) {
  LLVMContext &Ctx = M.getContext();
  std::vector<std::string> Params = collectParams(Inst);

  std::vector<Type *> ParamTypes(Params.size(), Type::getInt64Ty(Ctx));
  FunctionType *FTy = FunctionType::get(Type::getInt64Ty(Ctx), ParamTypes, false);
  Function *F = Function::Create(FTy, GlobalValue::ExternalLinkage,
                                 sanitizeName(Prefix), M);

  FunctionEmitter FE(Ctx, *F);
  unsigned I = 0;
  for (Argument &A : F->args()) {
    A.setName(Params[I]);
    FE.setParam(Params[I], &A);
    ++I;
  }
  FE.emitBehavior(Inst.Behavior);
}

} // namespace

std::string targetgen2::toLLVMIR(const Description &D) {
  LLVMContext Ctx;
  Module M("target_gen2", Ctx);

  for (size_t I = 0; I < D.Imports.size(); ++I)
    addCStringGlobal(M, ("tg2.import." + std::to_string(I)).c_str(), D.Imports[I]);

  for (const InstructionSetDef &IS : D.InstructionSets) {
    std::string ISAName = sanitizeName(IS.Name);
    addCStringGlobal(M, ("tg2.isa.name." + ISAName).c_str(), IS.Name);
    for (const llvm::targetgen2::Instruction &Inst : IS.ISA.Instructions)
      emitInstructionBehavior(M,
                              "tg2.exec.inst." + ISAName + "." + sanitizeName(Inst.Name),
                              Inst);
  }

  for (const CoreDef &Core : D.Cores) {
    std::string CoreName = sanitizeName(Core.Name);
    addCStringGlobal(M, ("tg2.core.name." + CoreName).c_str(), Core.Name);
    for (const llvm::targetgen2::Instruction &Inst : Core.ISA.Instructions)
      emitInstructionBehavior(
          M, "tg2.exec.core." + CoreName + "." + sanitizeName(Inst.Name), Inst);
  }

  if (verifyModule(M, &errs()))
    errs() << "target-gen2: generated invalid LLVM IR\n";

  std::string Out;
  raw_string_ostream OS(Out);
  M.print(OS, nullptr);
  return OS.str();
}
