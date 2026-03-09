//===- TargetGen2IR.cpp - CoreDSL LLVM IR emission --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TargetGen2IR.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;
using namespace llvm::targetgen2;

namespace {

struct FlattenedExpr {
  const Expr *E = nullptr;
  int32_t Parent = -1;
};

struct FlattenedStmt {
  const Statement *S = nullptr;
  int32_t Parent = -1;
};

static int32_t toInt(ExprKind K) { return static_cast<int32_t>(K); }
static int32_t toInt(ExprOp O) { return static_cast<int32_t>(O); }
static int32_t toInt(StatementKind K) { return static_cast<int32_t>(K); }

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

Constant *makeTextPtr(Module &M, StringRef Prefix, uint32_t Index, StringRef Text) {
  LLVMContext &Ctx = M.getContext();
  if (Text.empty())
    return ConstantPointerNull::get(PointerType::get(Ctx, 0));

  std::string Label = (Prefix + ".txt." + std::to_string(Index)).str();
  GlobalVariable *TextGV = addCStringGlobal(M, Label, Text);

  Constant *Zero = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
  Constant *Idx[] = {Zero, Zero};
  return ConstantExpr::getInBoundsGetElementPtr(TextGV->getValueType(), TextGV,
                                                Idx);
}

void flattenExpr(const Expr &E, int32_t Parent, std::vector<FlattenedExpr> &Out) {
  int32_t Self = static_cast<int32_t>(Out.size());
  Out.push_back({&E, Parent});
  for (const Expr &Child : E.Children)
    flattenExpr(Child, Self, Out);
}

void flattenStmt(const Statement &S, int32_t Parent, std::vector<FlattenedStmt> &Out,
                 std::vector<FlattenedExpr> &ExprOut) {
  int32_t Self = static_cast<int32_t>(Out.size());
  Out.push_back({&S, Parent});

  for (const Expr &E : S.Expressions)
    flattenExpr(E, Self, ExprOut);
  for (const Statement &Child : S.Children)
    flattenStmt(Child, Self, Out, ExprOut);
}

void addBehaviorGlobals(Module &M, StringRef Prefix, const Statement &Root) {
  LLVMContext &Ctx = M.getContext();

  std::vector<FlattenedStmt> Stmts;
  std::vector<FlattenedExpr> Exprs;
  flattenStmt(Root, -1, Stmts, Exprs);

  Type *I32 = Type::getInt32Ty(Ctx);
  Type *Ptr = PointerType::get(Ctx, 0);
  StructType *StmtTy = StructType::get(I32, I32, Ptr);
  StructType *ExprTy = StructType::get(I32, I32, I32, Ptr);

  std::vector<Constant *> StmtInits;
  StmtInits.reserve(Stmts.size());
  for (uint32_t I = 0; I < Stmts.size(); ++I) {
    const Statement &S = *Stmts[I].S;
    Constant *Fields[] = {
        ConstantInt::get(I32, toInt(S.Kind)),
        ConstantInt::get(I32, static_cast<uint32_t>(Stmts[I].Parent + 1)),
        makeTextPtr(M, Prefix, I, S.Text),
    };
    StmtInits.push_back(ConstantStruct::get(StmtTy, Fields));
  }

  std::vector<Constant *> ExprInits;
  ExprInits.reserve(Exprs.size());
  for (uint32_t I = 0; I < Exprs.size(); ++I) {
    const Expr &E = *Exprs[I].E;
    std::string ExprPrefix = (Prefix + ".expr").str();
    Constant *Fields[] = {
        ConstantInt::get(I32, toInt(E.Kind)),
        ConstantInt::get(I32, toInt(E.Op)),
        ConstantInt::get(I32, static_cast<uint32_t>(Exprs[I].Parent + 1)),
        makeTextPtr(M, ExprPrefix, I, E.Value.empty() ? E.Text : E.Value),
    };
    ExprInits.push_back(ConstantStruct::get(ExprTy, Fields));
  }

  ArrayType *StmtArrTy = ArrayType::get(StmtTy, StmtInits.size());
  ArrayType *ExprArrTy = ArrayType::get(ExprTy, ExprInits.size());

  auto *StmtGV = new GlobalVariable(
      M, StmtArrTy, true, GlobalValue::PrivateLinkage,
      ConstantArray::get(StmtArrTy, StmtInits), (Prefix + ".stmt_nodes").str());
  StmtGV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);

  auto *ExprGV = new GlobalVariable(
      M, ExprArrTy, true, GlobalValue::PrivateLinkage,
      ConstantArray::get(ExprArrTy, ExprInits), (Prefix + ".expr_nodes").str());
  ExprGV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);

  addCStringGlobal(M, (Prefix + ".summary").str(),
                   ("stmt_count=" + std::to_string(Stmts.size()) +
                    ",expr_count=" + std::to_string(Exprs.size()))
                       .c_str());
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
    for (const Instruction &Inst : IS.ISA.Instructions) {
      std::string InstName = sanitizeName(Inst.Name);
      std::string Prefix = "tg2.inst." + ISAName + "." + InstName;
      addCStringGlobal(M, Prefix + ".name", Inst.Name);
      addBehaviorGlobals(M, Prefix + ".behavior", Inst.Behavior);
    }
  }

  for (const CoreDef &Core : D.Cores) {
    std::string CoreName = sanitizeName(Core.Name);
    addCStringGlobal(M, ("tg2.core.name." + CoreName).c_str(), Core.Name);
    for (const Instruction &Inst : Core.ISA.Instructions) {
      std::string InstName = sanitizeName(Inst.Name);
      std::string Prefix = "tg2.core.inst." + CoreName + "." + InstName;
      addCStringGlobal(M, Prefix + ".name", Inst.Name);
      addBehaviorGlobals(M, Prefix + ".behavior", Inst.Behavior);
    }
    for (const AlwaysBlock &Always : Core.ISA.AlwaysBlocks) {
      std::string AlwaysName = sanitizeName(Always.Name);
      std::string Prefix = "tg2.core.always." + CoreName + "." + AlwaysName;
      addCStringGlobal(M, Prefix + ".name", Always.Name);
      addBehaviorGlobals(M, Prefix + ".behavior", Always.Behavior);
    }
  }

  FunctionType *FnTy = FunctionType::get(Type::getInt32Ty(Ctx), false);
  Function *Fn = Function::Create(FnTy, GlobalValue::ExternalLinkage,
                                  "tg2_emit_metadata", M);
  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", Fn);
  IRBuilder<> B(Entry);
  B.CreateRet(ConstantInt::get(
      Type::getInt32Ty(Ctx),
      static_cast<uint32_t>(D.Cores.size() + D.InstructionSets.size())));

  std::string Out;
  raw_string_ostream OS(Out);
  M.print(OS, nullptr);
  return OS.str();
}
