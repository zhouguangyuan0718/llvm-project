//===- GMIRPointerInfoTest.cpp -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "GISelMITest.h"
#include "llvm/CodeGen/GlobalISel/GMIRPointerInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::GISelAddressing;

namespace {

static constexpr LLT S1 = LLT::scalar(1);
static constexpr LLT S64 = LLT::scalar(64);
static constexpr LLT P0 = LLT::pointer(0, 64);

static void expectConstantOffset(const GMIRPointerInfo &Info, Register Base,
                                 int64_t Constant) {
  EXPECT_EQ(Info.getBase(), Base);
  EXPECT_TRUE(Info.getOffset().getTerms().empty());
  EXPECT_EQ(Info.getOffset().getConstant(), Constant);
}

static void expectOneTermOffset(const GMIRPointerInfo &Info, Register Base,
                                Register Reg, int64_t Scale,
                                int64_t Constant) {
  EXPECT_EQ(Info.getBase(), Base);
  ASSERT_EQ(Info.getOffset().getTerms().size(), 1u);
  EXPECT_EQ(Info.getOffset().getTerms()[0].Reg, Reg);
  EXPECT_EQ(Info.getOffset().getTerms()[0].Scale, Scale);
  EXPECT_EQ(Info.getOffset().getConstant(), Constant);
}

TEST_F(AArch64GISelMITest, RecursivePtrAddAndLinearOffset) {
  setUp();
  if (!TM)
    GTEST_SKIP();

  Register Base = B.buildUndef(P0).getReg(0);
  Register Index = B.buildUndef(S64).getReg(0);
  Register Four = B.buildConstant(S64, 4).getReg(0);
  Register Sixteen = B.buildConstant(S64, 16).getReg(0);
  Register Eight = B.buildConstant(S64, 8).getReg(0);

  Register Scaled = B.buildInstr(TargetOpcode::G_MUL, {S64}, {Index, Four})
                        .getReg(0);
  Register Offset = B.buildInstr(TargetOpcode::G_ADD, {S64}, {Scaled, Sixteen})
                        .getReg(0);
  Register Ptr0 = B.buildPtrAdd(P0, Base, Offset).getReg(0);
  Register Ptr1 = B.buildPtrAdd(P0, Ptr0, Eight).getReg(0);

  GMIRPointerAnalyzer Analyzer(*MRI);
  GMIRPointerInfo Info = Analyzer.getPointerInfo(Ptr1);

  expectOneTermOffset(Info, Base, Index, 4, 24);
}

TEST_F(AArch64GISelMITest, WithAddedRegAnalyzesOffsetRegister) {
  setUp();
  if (!TM)
    GTEST_SKIP();

  Register Base = B.buildUndef(P0).getReg(0);
  Register Cst = B.buildConstant(S64, 16).getReg(0);

  GMIRPointerAnalyzer Analyzer(*MRI);
  GMIRPointerInfo BaseInfo(Base);

  GMIRPointerInfo PtrPlusCst = Analyzer.withAddedReg(BaseInfo, Cst, 2);
  expectConstantOffset(PtrPlusCst, Base, 32);

  Register Index = B.buildUndef(S64).getReg(0);
  Register Four = B.buildConstant(S64, 4).getReg(0);
  Register Sixteen = B.buildConstant(S64, 16).getReg(0);
  Register Scaled = B.buildInstr(TargetOpcode::G_MUL, {S64}, {Index, Four})
                        .getReg(0);
  Register Offset = B.buildInstr(TargetOpcode::G_ADD, {S64}, {Scaled, Sixteen})
                        .getReg(0);

  GMIRPointerInfo PtrPlusLinear = Analyzer.withAddedReg(BaseInfo, Offset, 2);
  expectOneTermOffset(PtrPlusLinear, Base, Index, 8, 32);
}

TEST_F(AArch64GISelMITest, SelectWithSameInputs) {
  setUp();
  if (!TM)
    GTEST_SKIP();

  Register Base = B.buildUndef(P0).getReg(0);
  Register Index = B.buildUndef(S64).getReg(0);
  Register Four = B.buildConstant(S64, 4).getReg(0);
  Register Sixteen = B.buildConstant(S64, 16).getReg(0);
  Register Cond = B.buildConstant(S1, 1).getReg(0);

  Register Scaled = B.buildInstr(TargetOpcode::G_MUL, {S64}, {Index, Four})
                        .getReg(0);
  Register Offset = B.buildInstr(TargetOpcode::G_ADD, {S64}, {Scaled, Sixteen})
                        .getReg(0);
  Register Selected =
      B.buildInstr(TargetOpcode::G_SELECT, {S64}, {Cond, Offset, Offset})
          .getReg(0);
  Register Ptr = B.buildPtrAdd(P0, Base, Selected).getReg(0);

  GMIRPointerAnalyzer Analyzer(*MRI);
  GMIRPointerInfo Info = Analyzer.getPointerInfo(Ptr);

  expectOneTermOffset(Info, Base, Index, 4, 16);
}

TEST_F(AArch64GISelMITest, PhiWithSameInputs) {
  setUp();
  if (!TM)
    GTEST_SKIP();

  Register Base = B.buildUndef(P0).getReg(0);
  MachineInstrBuilder Phi = B.buildInstr(TargetOpcode::G_PHI, {P0}, {});
  Phi.addReg(Base).addMBB(EntryMBB).addReg(Base).addMBB(EntryMBB);

  GMIRPointerAnalyzer Analyzer(*MRI);
  GMIRPointerInfo Info = Analyzer.getPointerInfo(Phi.getReg(0));

  expectConstantOffset(Info, Base, 0);
}

} // namespace
