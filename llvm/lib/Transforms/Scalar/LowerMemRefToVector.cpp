//===- LowerMemRefToVector.cpp - Lower memref intrinsics to vectors -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/LowerMemRefToVector.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Transforms/Scalar/LowerMemRefCommon.h"
#include "llvm/Transforms/Scalar/LowerMemRefIntrinsicInfo.h"

using namespace llvm;
using namespace llvm::memref_lowering;

namespace {

static bool decodeVectorCandidate(CallInst *CI, DecodedCall &DC) {
  if (!CI)
    return false;

  Function *Callee = CI->getCalledFunction();
  if (!Callee)
    return false;

  Intrinsic::ID ID = Callee->getIntrinsicID();

  if (ID == Intrinsic::not_intrinsic)
    return false;

  OperandRoles Roles;
  if (!getIntrinsicOperandRoles(ID, CI, Roles))
    return false;

  return decodeCallWithOperandRoles(CI, Roles, DC);
}

static bool lowerOneCall(CallInst *CI) {
  DecodedCall DC;

  if (!decodeVectorCandidate(CI, DC))
    return false;

  IRBuilder<> B(CI);

  VectorType *VecTy = getFixedVectorTy(DC.ElemTy, DC.NumElems);

  SmallVector<Value *, 4> InputVecs;
  InputVecs.reserve(DC.Inputs.size());

  for (unsigned I = 0; I < DC.Inputs.size(); ++I) {
    const DecodedMemRef &InputMR = DC.Inputs[I];

    Value *Ptr = emitBaseOffsetPtr(B, DC.ElemTy, InputMR,
                                   Twine("in") + Twine(I));

    LoadInst *Vec = B.CreateLoad(VecTy, Ptr,
                                 Twine("in") + Twine(I) + ".vec");

    InputVecs.push_back(Vec);
  }

  SmallVector<Value *, 2> OutputVecs;

  if (!emitVectorIntrinsicSemantics(B, DC.IntrinsicID, InputVecs,
                                    DC.Outputs.size(), OutputVecs))
    return false;

  if (OutputVecs.size() != DC.Outputs.size())
    return false;

  for (unsigned I = 0; I < DC.Outputs.size(); ++I) {
    const DecodedMemRef &OutputMR = DC.Outputs[I];

    Value *Ptr = emitBaseOffsetPtr(B, DC.ElemTy, OutputMR,
                                   Twine("out") + Twine(I));

    B.CreateStore(OutputVecs[I], Ptr);
  }

  CI->eraseFromParent();
  return true;
}

} // namespace

PreservedAnalyses
llvm::LowerMemRefToVectorPass::run(Function &F, FunctionAnalysisManager &AM) {
  SmallVector<CallInst *, 16> Worklist;

  for (Instruction &I : instructions(F)) {
    auto *CI = dyn_cast<CallInst>(&I);
    if (!CI)
      continue;

    Function *Callee = CI->getCalledFunction();
    if (!Callee)
      continue;

    if (Callee->getIntrinsicID() == Intrinsic::not_intrinsic)
      continue;

    Worklist.push_back(CI);
  }

  bool Changed = false;

  for (CallInst *CI : Worklist)
    Changed |= lowerOneCall(CI);

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
