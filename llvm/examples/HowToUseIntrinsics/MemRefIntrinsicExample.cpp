//===- MemRefIntrinsicExample.cpp - Use memref intrinsics via API --------===//
//
// This file demonstrates creating a call to:
//   llvm.memref.elem.add.rank2
// and printing the resulting IR.
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/Constants.h"
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

  // int_memref_elem_add_rank2 is a concrete (non-overloaded) intrinsic.
  // So we must not pass overloaded type parameters here.
  Function *Intr =
      Intrinsic::getOrInsertDeclaration(&M, Intrinsic::memref_elem_add_rank2);

  // Always use the exact parameter type from the declaration to avoid
  // signature mismatches across type-system/ABI evolution.
  Type *MemRefDescTy = Intr->getFunctionType()->getParamType(0);

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
