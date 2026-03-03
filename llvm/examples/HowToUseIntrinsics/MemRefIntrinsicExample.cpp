//===- MemRefIntrinsicExample.cpp - Use memref intrinsics via API --------===//
//
// This file demonstrates creating a call to:
//   llvm.memref.elem.add.rank2
// and printing the resulting IR.
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

int main() {
  LLVMContext Ctx;
  Module M("memref_intrinsic_demo", Ctx);
  IRBuilder<> B(Ctx);

  FunctionType *FT = FunctionType::get(Type::getVoidTy(Ctx), false);
  Function *F = Function::Create(FT, Function::ExternalLinkage, "demo", M);
  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", F);
  B.SetInsertPoint(Entry);

  // Rank-2 memref descriptor type:
  // { ptr, ptr, i64, [2 x i64], [2 x i64] }
  Type *I64 = Type::getInt64Ty(Ctx);
  Type *Ptr = PointerType::get(Ctx, 0);
  Type *Arr2I64 = ArrayType::get(I64, 2);
  StructType *MemRefDescTy =
      StructType::get(Ctx, {Ptr, Ptr, I64, Arr2I64, Arr2I64});

  Function *Intr = Intrinsic::getOrInsertDeclaration(
      &M, Intrinsic::memref_elem_add_rank2,
      {MemRefDescTy, MemRefDescTy, MemRefDescTy});

  Value *A = PoisonValue::get(MemRefDescTy);
  Value *Bv = PoisonValue::get(MemRefDescTy);
  Value *C = PoisonValue::get(MemRefDescTy);
  B.CreateCall(Intr, {A, Bv, C});
  B.CreateRetVoid();

  if (verifyModule(M, &errs())) {
    errs() << "module verification failed\n";
    return 1;
  }

  M.print(outs(), nullptr);
  return 0;
}
