//===- LowerMemRefReductionToScalar.cpp - Lower memref reductions --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/LowerMemRefReductionToScalar.h"

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

struct DecodedReductionCall {
  CallInst *CI = nullptr;
  MemRefReductionKind Kind = MemRefReductionKind::Add;
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

  if (Output.Rank > Input.Rank || Input.Rank - Output.Rank > 1)
    return false;

  if (Input.Rank == 1) {
    if (ReduceDim != 0 || Output.Rank != 1)
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

  // The result contains one element, so the non-reduced dimension must be one.
  if (RemainingSize != 1)
    return false;

  ReduceLength = ReducedSize;
  ReduceStride = ReduceDim == 0 ? Input.Stride0 : Input.Stride1;
  return true;
}

static bool decodeReductionCandidate(CallInst *CI, DecodedReductionCall &DC) {
  if (!CI || !CI->getType()->isVoidTy())
    return false;

  Function *Callee = CI->getCalledFunction();
  if (!Callee)
    return false;

  MemRefReductionKind Kind;
  if (!getMemRefReductionKind(Callee->getIntrinsicID(), Kind))
    return false;

  // Fixed intrinsic ABI:
  //   arg0: input memref
  //   arg1: output memref
  //   arg2: reduced dimension
  if (CI->arg_size() != 3)
    return false;

  DecodedMemRef Input;
  if (!decodeMemRef(CI->getArgOperand(0), 0, Input,
                    /*RequireContiguous=*/false))
    return false;

  DecodedMemRef Output;
  if (!decodeMemRef(CI->getArgOperand(1), 1, Output,
                    /*RequireContiguous=*/false))
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
  DC.Kind = Kind;
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
    Delta = B.CreateMul(Index, Stride, "reduction.elem.delta");
  }

  Value *ElemOffset =
      B.CreateAdd(BaseOffset, Delta, "reduction.elem.offset");
  return B.CreateGEP(ElemTy, AlignedPtr, ElemOffset, "reduction.elem.ptr");
}

static Value *emitReductionStep(IRBuilder<> &B, MemRefReductionKind Kind,
                                Value *Accumulator, Value *Element) {
  switch (Kind) {
  case MemRefReductionKind::Add:
    return B.CreateFAdd(Accumulator, Element, "reduction.next.acc");
  case MemRefReductionKind::Mul:
    return B.CreateFMul(Accumulator, Element, "reduction.next.acc");
  case MemRefReductionKind::Maximum:
    return B.CreateMaximum(Accumulator, Element, "reduction.next.acc");
  case MemRefReductionKind::Minimum:
    return B.CreateMinimum(Accumulator, Element, "reduction.next.acc");
  }

  llvm_unreachable("unknown memref reduction kind");
}

static bool lowerSingleElementReduction(const DecodedReductionCall &DC) {
  CallInst *CI = DC.CI;
  IRBuilder<> B(CI);
  Type *HalfTy = Type::getHalfTy(CI->getContext());

  Value *InputAligned =
      emitAlignedPtr(B, DC.Input, "reduction.input.aligned");
  Value *InputOffset = emitOffset(B, DC.Input, "reduction.input.offset");
  Value *InputPtr =
      B.CreateGEP(HalfTy, InputAligned, InputOffset, "reduction.input.ptr");
  Value *Result = B.CreateLoad(HalfTy, InputPtr, "reduction.result");

  Value *OutputPtr =
      emitBaseOffsetPtr(B, HalfTy, DC.Output, "reduction.output");
  B.CreateStore(Result, OutputPtr);

  CI->eraseFromParent();
  return true;
}

static bool lowerOneCall(CallInst *CI) {
  DecodedReductionCall DC;
  if (!decodeReductionCandidate(CI, DC))
    return false;

  if (DC.ReduceLength == 1)
    return lowerSingleElementReduction(DC);

  LLVMContext &Ctx = CI->getContext();
  Function *F = CI->getFunction();
  Type *HalfTy = Type::getHalfTy(Ctx);

  BasicBlock *PreheaderBB = CI->getParent();
  BasicBlock *AfterBB =
      PreheaderBB->splitBasicBlock(CI, "memref.reduction.after");
  BasicBlock *LoopBB =
      BasicBlock::Create(Ctx, "memref.reduction.loop", F, AfterBB);
  BasicBlock *ExitBB =
      BasicBlock::Create(Ctx, "memref.reduction.exit", F, AfterBB);

  PreheaderBB->getTerminator()->eraseFromParent();

  IRBuilder<> PreB(PreheaderBB);
  Value *InputAligned =
      emitAlignedPtr(PreB, DC.Input, "reduction.input.aligned");
  Value *InputBaseOffset =
      emitOffset(PreB, DC.Input, "reduction.input.offset");
  Value *OutputPtr =
      emitBaseOffsetPtr(PreB, HalfTy, DC.Output, "reduction.output");

  auto *IndexTy = cast<IntegerType>(InputBaseOffset->getType());
  Value *ZeroIndex = ConstantInt::get(IndexTy, 0);
  Value *OneIndex = ConstantInt::get(IndexTy, 1);
  Value *EndIndex = ConstantInt::get(IndexTy, DC.ReduceLength);

  Value *FirstElemPtr = emitInputElementPtr(
      PreB, HalfTy, InputAligned, InputBaseOffset, ZeroIndex, DC.ReduceStride);
  Value *InitialAccumulator =
      PreB.CreateLoad(HalfTy, FirstElemPtr, "reduction.initial.acc");
  PreB.CreateBr(LoopBB);

  IRBuilder<> LoopB(LoopBB);
  PHINode *IndexPhi = LoopB.CreatePHI(IndexTy, 2, "reduction.iv");
  PHINode *AccumulatorPhi =
      LoopB.CreatePHI(HalfTy, 2, "reduction.acc");

  IndexPhi->addIncoming(OneIndex, PreheaderBB);
  AccumulatorPhi->addIncoming(InitialAccumulator, PreheaderBB);

  Value *ElemPtr = emitInputElementPtr(
      LoopB, HalfTy, InputAligned, InputBaseOffset, IndexPhi, DC.ReduceStride);
  Value *Element = LoopB.CreateLoad(HalfTy, ElemPtr, "reduction.elem");
  Value *NextAccumulator =
      emitReductionStep(LoopB, DC.Kind, AccumulatorPhi, Element);
  Value *NextIndex = LoopB.CreateAdd(IndexPhi, OneIndex, "reduction.next.iv");
  Value *Done = LoopB.CreateICmpEQ(NextIndex, EndIndex, "reduction.done");
  LoopB.CreateCondBr(Done, ExitBB, LoopBB);

  IndexPhi->addIncoming(NextIndex, LoopBB);
  AccumulatorPhi->addIncoming(NextAccumulator, LoopBB);

  IRBuilder<> ExitB(ExitBB);
  ExitB.CreateStore(NextAccumulator, OutputPtr);
  ExitB.CreateBr(AfterBB);

  CI->eraseFromParent();
  return true;
}

} // namespace

PreservedAnalyses llvm::LowerMemRefReductionToScalarPass::run(
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

    MemRefReductionKind Kind;
    if (getMemRefReductionKind(Callee->getIntrinsicID(), Kind))
      Worklist.push_back(CI);
  }

  bool Changed = false;
  for (CallInst *CI : Worklist)
    Changed |= lowerOneCall(CI);

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
