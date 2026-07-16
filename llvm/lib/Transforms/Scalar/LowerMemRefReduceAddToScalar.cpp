//===- LowerMemRefReduceAddToScalar.cpp ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/LowerMemRefReduceAddToScalar.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
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

struct DecodedReduceAddCall {
  CallInst *CI = nullptr;
  DecodedMemRef Input;
  bool HasOutputArg = false;
  bool OutputIsMemRef = false;
  Value *OutputPtr = nullptr;
  DecodedMemRef OutputMemRef;
};

static bool decodeReduceAddCandidate(CallInst *CI, DecodedReduceAddCall &DC) {
  if (!CI)
    return false;

  Function *Callee = CI->getCalledFunction();
  if (!Callee)
    return false;

  Intrinsic::ID ID = Callee->getIntrinsicID();
  if (ID == Intrinsic::not_intrinsic)
    return false;

  ReduceAddOperandRole Role;
  if (!getReduceAddOperandRole(ID, CI, Role))
    return false;

  if (Role.InputArgIndex == InvalidArgIndex ||
      Role.InputArgIndex >= CI->arg_size())
    return false;

  DecodedMemRef InputMR;
  if (!decodeMemRef(CI->getArgOperand(Role.InputArgIndex),
                    Role.InputArgIndex, InputMR))
    return false;

  Type *HalfTy = Type::getHalfTy(CI->getContext());

  DC.CI = CI;
  DC.Input = InputMR;

  if (Role.OutputArgIndex == InvalidArgIndex) {
    if (CI->getType() != HalfTy)
      return false;
    DC.HasOutputArg = false;
    return true;
  }

  if (Role.OutputArgIndex >= CI->arg_size())
    return false;

  if (!CI->getType()->isVoidTy())
    return false;

  DC.HasOutputArg = true;
  DC.OutputIsMemRef = Role.OutputIsMemRef;

  Value *OutputArg = CI->getArgOperand(Role.OutputArgIndex);

  if (Role.OutputIsMemRef) {
    DecodedMemRef OutputMR;
    if (!decodeMemRef(OutputArg, Role.OutputArgIndex, OutputMR))
      return false;
    if (OutputMR.NumElems != 1)
      return false;
    DC.OutputMemRef = OutputMR;
    return true;
  }

  if (!OutputArg->getType()->isPointerTy())
    return false;

  DC.OutputPtr = OutputArg;
  return true;
}

static Value *emitInputElementPtr(IRBuilder<> &B, Type *ElemTy,
                                  const DecodedMemRef &MR, Value *Index) {
  Value *AlignedPtr = emitAlignedPtr(B, MR, "reduce.in.aligned");
  Value *BaseOffset = emitOffset(B, MR, "reduce.in.offset");

  Value *ElemOffset = nullptr;
  if (MR.FlattenStride == 1) {
    ElemOffset = B.CreateAdd(BaseOffset, Index, "reduce.elem.offset");
  } else {
    Value *Stride = ConstantInt::get(Index->getType(), MR.FlattenStride);
    Value *Delta = B.CreateMul(Index, Stride, "reduce.elem.delta");
    ElemOffset = B.CreateAdd(BaseOffset, Delta, "reduce.elem.offset");
  }

  return B.CreateGEP(ElemTy, AlignedPtr, ElemOffset, "reduce.elem.ptr");
}

static bool lowerOneCall(CallInst *CI) {
  DecodedReduceAddCall DC;
  if (!decodeReduceAddCandidate(CI, DC))
    return false;

  LLVMContext &Ctx = CI->getContext();
  Function *F = CI->getFunction();
  Type *HalfTy = Type::getHalfTy(Ctx);

  BasicBlock *PreheaderBB = CI->getParent();
  BasicBlock *AfterBB = PreheaderBB->splitBasicBlock(CI, "memref.reduce.after");
  BasicBlock *LoopBB = BasicBlock::Create(Ctx, "memref.reduce.loop", F,
                                          AfterBB);
  BasicBlock *ExitBB = BasicBlock::Create(Ctx, "memref.reduce.exit", F,
                                          AfterBB);

  PreheaderBB->getTerminator()->eraseFromParent();

  IRBuilder<> PreB(PreheaderBB);
  Value *InputBaseOffset = emitOffset(PreB, DC.Input, "reduce.input.offset");
  Type *IndexTy = InputBaseOffset->getType();
  Value *ZeroIndex = ConstantInt::get(IndexTy, 0);
  Value *EndIndex = ConstantInt::get(IndexTy, DC.Input.NumElems);
  Value *InitialAcc = ConstantFP::get(HalfTy, 0.0);

  Value *OutputPtr = nullptr;
  if (DC.HasOutputArg) {
    if (DC.OutputIsMemRef)
      OutputPtr = emitBaseOffsetPtr(PreB, HalfTy, DC.OutputMemRef,
                                    "reduce.out");
    else
      OutputPtr = DC.OutputPtr;
  }

  PreB.CreateBr(LoopBB);

  IRBuilder<> LoopB(LoopBB);
  PHINode *IndexPhi = LoopB.CreatePHI(IndexTy, 2, "reduce.iv");
  PHINode *AccPhi = LoopB.CreatePHI(HalfTy, 2, "reduce.acc");

  IndexPhi->addIncoming(ZeroIndex, PreheaderBB);
  AccPhi->addIncoming(InitialAcc, PreheaderBB);

  Value *ElemPtr = emitInputElementPtr(LoopB, HalfTy, DC.Input, IndexPhi);
  LoadInst *Elem = LoopB.CreateLoad(HalfTy, ElemPtr, "reduce.elem");
  Value *NextAcc = LoopB.CreateFAdd(AccPhi, Elem, "reduce.next.acc");
  Value *One = ConstantInt::get(IndexTy, 1);
  Value *NextIndex = LoopB.CreateAdd(IndexPhi, One, "reduce.next.iv");
  Value *Done = LoopB.CreateICmpEQ(NextIndex, EndIndex, "reduce.done");
  LoopB.CreateCondBr(Done, ExitBB, LoopBB);

  IndexPhi->addIncoming(NextIndex, LoopBB);
  AccPhi->addIncoming(NextAcc, LoopBB);

  IRBuilder<> ExitB(ExitBB);
  if (DC.HasOutputArg)
    ExitB.CreateStore(NextAcc, OutputPtr);
  ExitB.CreateBr(AfterBB);

  if (!DC.HasOutputArg)
    CI->replaceAllUsesWith(NextAcc);
  CI->eraseFromParent();
  return true;
}

} // namespace

PreservedAnalyses llvm::LowerMemRefReduceAddToScalarPass::run(
    Function &F, FunctionAnalysisManager &AM) {
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
