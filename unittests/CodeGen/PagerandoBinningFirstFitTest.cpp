//===-- PagerandoBinningSimpleTest.cpp ------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/PagerandoBinning.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {

struct PagerandoBinningFirstFitTest : public testing::Test {
  PagerandoBinnerBase::FirstFitAlgo Algo;

  void ASSERT_ASSIGNMENTS(
      std::initializer_list<std::pair<unsigned, unsigned>> Assignments) {
    for (auto &A : Assignments) {
      unsigned FnSize, ExpectedBin;
      std::tie(FnSize, ExpectedBin) = A;
      unsigned Bin = Algo.assignToBin(FnSize);
      ASSERT_EQ(Bin, ExpectedBin);
    }
  }
};

TEST_F(PagerandoBinningFirstFitTest, NeverReturnsDefaultBin) {
  ASSERT_NE(Algo.assignToBin(100), 0u);
}

TEST_F(PagerandoBinningFirstFitTest, UsesGreedyAlgorithm) {
  ASSERT_ASSIGNMENTS({
      {3000, 1},
      {3000, 2},
      {1000, 1},
      {1000, 2},
      {1000, 3},
  });
}

TEST_F(PagerandoBinningFirstFitTest, UsesRemainingFreeSpace) {
  ASSERT_ASSIGNMENTS({
      {3000, 1},
      {1000, 1},
      { 100, 2},
      {  90, 1},
      {   6, 1},
      {   1, 2},
  });
}

TEST_F(PagerandoBinningFirstFitTest, UsesBinWithLeastFreeSpace) {
  ASSERT_ASSIGNMENTS({
      {3000, 1},
      {3001, 2},
      {3000, 3},
      { 100, 2},
  });
}

TEST_F(PagerandoBinningFirstFitTest, FreeSpaceMustBeAtLeastMinFnSize) {
  ASSERT_ASSIGNMENTS({
      {4095, 1},
      {   1, 2},
      {4095, 2},
  });
}

TEST_F(PagerandoBinningFirstFitTest, BinSizedFunctionsAlwaysGetTheirOwnBin) {
  ASSERT_ASSIGNMENTS({
      {4096, 1},
      {8192, 2},
      {   1, 3},
  });
}

TEST_F(PagerandoBinningFirstFitTest, LargeFunctionsAreStillPacked) {
  ASSERT_ASSIGNMENTS({
     {8000, 1},
     { 100, 1},
  });
}

} // end anonymous namespace
