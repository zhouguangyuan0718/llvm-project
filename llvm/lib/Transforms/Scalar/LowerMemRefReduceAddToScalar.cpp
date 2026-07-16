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
  DecodedMemRef Output;
  unsigned ReduceDim = 0;
  uint64_t ReduceLength = 0;
  uint64_t ReduceStride = 1;
};

static bool isOneElementOutput(const DecodedMemRef &Output) {
  if (Output.NumElems != 1)
    return false;

  if (Output.Rank == 1)
    return Output.Size0 == 1;

  if (Output.Rank == 2)
    return Output.Size0 == 1 && Output.Size1 == 1;

  return false;
}

static bool decodeReductionShape(const DecodedMemRef &Input,
                                 const DecodedMemRef &Output,
                                 unsigned ReduceDim,
                                 uint64_t &ReduceLength,
                                 uint64_t &ReduceStride) {
  if (!isOneElementOutput(Output))
    return false;

  if (Output.Rank > Input.Rank)
    return false;

  if (Input.Rank - Output.Rank > 1)
    return false;

  if (Input.Rank == 1) {
    if (ReduceDim != 0)
      return false;

    if (Output.Rank != 1)
      return false;

    ReduceLength = Input.Size0;
    ReduceStride = Input.Stride0;
    return true;
  }

  if (Input.Rank != 2 || ReduceDim > 1)
    return false;

  if (Output.Rank != 1 && Output.Rank != 2)
    return false;

  uint64_t ReducedSize = ReduceDim == 0 ? Input.Size0 : Input.Size1;
  uint64_t RemainingSize = ReduceDim == 0 ? Input.Size1 : Input.Size0;

  // The output contains one element, so the non-reduced dimension must have
  // size one. This also rejects reducing the unit dimension of [1, N] or
  // [N, 1], except for the [1, 1] case.
  if (RemainingSize != 1)
    return false;

  ReduceLength = ReducedSize;
  ReduceStride = ReduceDim == 0 ? Input.Stride0 : Input.Stride1;
  return true;
}

static bool decodeReduceAddCandidate(CallInst *CI, DecodedReduceAddCall &DC) {
  if (!CI || !CI->getType()->isVoidTy())
    return false;

  Function *Callee = CI->getCalledFunction();
  if (!Callee)
    return false;

  Intrinsic::ID ID = Callee->getIntrinsicID();
  if (!isMemRefReduceAddIntrinsic(ID))
    return false;

  // Fixed intrinsic ABI:
  //   arg0: input memref
  //   arg1: output memref
  //   arg2: reduced dimension
  if (CI->arg_size() != 3)
    return false;

  DecodedMemRef Input;
  if (!decodeMemRef(CI->getArgOperand(0), 0, Input))
    return false;

  DecodedMemRef Output;
  if (!decodeMemRef(CI->getArgOperand(1), 1, Output))
    return false;

  auto *DimCI = dyn_cast<ConstantInt>(CI->getArgOperand(2));
  if (!DimCI)
    return false;

  uint64_t DimValue = DimCI->getZExtValue();
  if (DimValue > 1)
    return false;

  uint64_t ReduceLength = 0;
  uint64_t ReduceStride = 1;
  if (!decodeReductionShape(Input, Output, static_cast<unsigned>(DimValue),
                            ReduceLength, ReduceStride))
    return false;

  if (ReduceLength == 0)
    return false;

  DC.CI = CI;
  DC.Input = Input;
  DC.Output = Output;
  DC.ReduceDim = static_cast<unsigned>(DimValue);
  DC.ReduceLength = ReduceLength;
  DC.ReduceStride = ReduceStride;
  return true;
}

static Value *emitInputElementPtr(IRBuilder<> &B, Type *ElemTy,
                                  Value *AlignedPtr, Value *BaseOffset,
                                  Value *Index, uint64_t ReduceStride) {
  Value *Delta = Index;

  if (ReduceStride != 1) {
    Value *Stride = ConstantInt::get(Index->getType(), ReduceStride);
    Delta = B.CreateMul(Index, Stride, "reduce.elem.delta");
  }

  Value *ElemOffset =
      B.CreateAdd(BaseOffset, Delta, "reduce.elem.offset");
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
  BasicBlock *AfterBB =
      PreheaderBB->splitBasicBlock(CI, "memref.reduce.after");
  BasicBlock *LoopBB =
      BasicBlock::Create(Ctx, "memref.reduce.loop", F, AfterBB);
  BasicBlock *ExitBB =
      BasicBlock::Create(Ctx, "memref.reduce.exit", F, AfterBB);

  PreheaderBB->getTerminator()->eraseFromParent();

  IRBuilder<> PreB(PreheaderBB);
  Value *InputAligned =
      emitAlignedPtr(PreB, DC.Input, "reduce.input.aligned");
  Value *InputBaseOffset =
      emitOffset(PreB, DC.Input, "reduce.input.offset");
  Value *OutputPtr =
      emitBaseOffsetPtr(PreB, HalfTy, DC.Output, "reduce.output");

  auto *IndexTy = cast<IntegerType>(InputBaseOffset->getType());
  Value *ZeroIndex = ConstantInt::get(IndexTy, 0);
  Value *OneIndex = ConstantInt::get(IndexTy, 1);
  Value *EndIndex = ConstantInt::get(IndexTy, DC.ReduceLength);
  Value *InitialAcc = ConstantFP::get(HalfTy, 0.0);

  PreB.CreateBr(LoopBB);

  IRBuilder<> LoopB(LoopBB);
  PHINode *IndexPhi = LoopB.CreatePHI(IndexTy, 2, "reduce.iv");
  PHINode *AccPhi = LoopB.CreatePHI(HalfTy, 2, "reduce.acc");

  IndexPhi->addIncoming(ZeroIndex, PreheaderBB);
  AccPhi->addIncoming(InitialAcc, PreheaderBB);

  Value *ElemPtr =
      emitInputElementPtr(LoopB, HalfTy, InputAligned, InputBaseOffset,
                          IndexPhi, DC.ReduceStride);
  LoadInst *Elem = LoopB.CreateLoad(HalfTy, ElemPtr, "reduce.elem");
  Value *NextAcc = LoopB.CreateFAdd(AccPhi, Elem, "reduce.next.acc");
  Value *NextIndex = LoopB.CreateAdd(IndexPhi, OneIndex, "reduce.next.iv");
  Value *Done = LoopB.CreateICmpEQ(NextIndex, EndIndex, "reduce.done");
  LoopB.CreateCondBr(Done, ExitBB, LoopBB);

  IndexPhi->addIncoming(NextIndex, LoopBB);
  AccPhi->addIncoming(NextAcc, LoopBB);

  IRBuilder<> ExitB(ExitBB);
  ExitB.CreateStore(NextAcc, OutputPtr);
  ExitB.CreateBr(AfterBB);

  CI->eraseFromParent();
  return true;
}

} // namespace

PreservedAnalyses llvm::LowerMemRefReduceAddToScalarPass::run(
    Function &F, FunctionAnalysisManager &AM) {
  (void)AM;

  SmallVector<CallInst *, 16> Worklist;

  for (Instruction &I : instructions(F)) {
    auto *CI = dyn_cast<CallInst>(&I);
    if (!CI)
      continue;

    Function *Callee = CI->getCalledFunction();
    if (!Callee)
      continue;

    if (isMemRefReduceAddIntrinsic(Callee->getIntrinsicID()))
      Worklist.push_back(CI);
  }

  bool Changed = false;
  for (CallInst *CI : Worklist)
    Changed |= lowerOneCall(CI);

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
