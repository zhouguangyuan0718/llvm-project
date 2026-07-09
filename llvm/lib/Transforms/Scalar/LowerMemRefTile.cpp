//===- LowerMemRefTile.cpp - Tile memref intrinsics -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/LowerMemRefTile.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Transforms/Scalar/LowerMemRefCommon.h"
#include "llvm/Transforms/Scalar/LowerMemRefIntrinsicInfo.h"

#include <algorithm>

using namespace llvm;
using namespace llvm::memref_lowering;

namespace {

static bool decodeTileCandidate(CallInst *CI, DecodedCall &DC) {
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

static bool tileOneCall(CallInst *CI, unsigned TileElems) {
  if (TileElems == 0)
    return false;

  DecodedCall DC;
  if (!decodeTileCandidate(CI, DC))
    return false;

  if (DC.NumElems <= TileElems)
    return false;

  Function *Callee = CI->getCalledFunction();
  if (!Callee)
    return false;

  IRBuilder<> B(CI);

  for (unsigned TileStart = 0; TileStart < DC.NumElems;
       TileStart += TileElems) {
    unsigned Remaining = DC.NumElems - TileStart;
    unsigned ThisTileSize = std::min(TileElems, Remaining);

    SmallVector<Value *, 8> NewArgs;
    NewArgs.reserve(CI->arg_size());

    for (Use &U : CI->args())
      NewArgs.push_back(U.get());

    for (const DecodedMemRef &MR : DC.AllMemRefs) {
      Value *TiledMR = buildTiledMemRef(
          B, MR, TileStart, ThisTileSize,
          Twine("tile") + Twine(TileStart) + ".arg" + Twine(MR.ArgIndex));

      NewArgs[MR.ArgIndex] = TiledMR;
    }

    CallInst *NewCI = B.CreateCall(Callee->getFunctionType(), Callee, NewArgs,
                                   Twine(CI->getName()) + ".tile");

    copyCallProperties(NewCI, CI);
  }

  CI->eraseFromParent();
  return true;
}

} // namespace

PreservedAnalyses
llvm::LowerMemRefTilePass::run(Function &F, FunctionAnalysisManager &AM) {
  const TargetTransformInfo &TTI = AM.getResult<TargetIRAnalysis>(F);
  unsigned TileElems = getTargetFP16VectorLanes(TTI);

  if (TileElems == 0)
    return PreservedAnalyses::all();

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
    Changed |= tileOneCall(CI, TileElems);

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
