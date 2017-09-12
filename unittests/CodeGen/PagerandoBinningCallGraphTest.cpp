//===-- PagerandoBinningCallGraphTest.cpp ---------------------------------===//
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

struct PagerandoBinningGallGraphTest : public testing::Test {
  PagerandoBinning::CallGraphAlgo Algo;
  typedef unsigned Id;

  void defineGraph(std::initializer_list<unsigned> Sizes,
                   std::initializer_list<std::pair<Id, Id>> Edges) {
    for (auto Size : Sizes) {
      Algo.addNode(Size);
    }

    // Edges need to be added bottom-up; use reverse iterator.
    // Replace with std::rbegin once we have C++14
    std::vector<std::pair<Id, Id>> Vec(Edges);
    for (auto &Edge : make_range(Vec.rbegin(), Vec.rend())) {
      Algo.addEdge(Edge.first, Edge.second);
    }
  }

  void ASSERT_ASSIGNMENTS(std::initializer_list<unsigned> ExpectedBins) {
    auto Bins = Algo.computeAssignments();
    ASSERT_EQ(Bins.size(), ExpectedBins.size());

    unsigned Id = 0;
    for (auto ExpectedBin : ExpectedBins) {
      unsigned Bin = Bins.at(Id++);
      ASSERT_EQ(Bin, ExpectedBin);
    }
  }
};

//TEST_F(PagerandoBinningSimpleTest, UsesGreedyAlgorithm) {
//  ASSERT_ASSIGNMENTS({
//      {3000, 1},
//      {3000, 2},
//      {1000, 1},
//      {1000, 2},
//      {1000, 3},
//  });
//}

//TEST_F(PagerandoBinningSimpleTest, UsesRemainingFreeSpace) {
//  ASSERT_ASSIGNMENTS({
//      {3000, 1},
//      {1000, 1},
//      { 100, 2},
//      {  90, 1},
//      {   6, 1},
//      {   1, 2},
//  });
//}
//
//TEST_F(PagerandoBinningSimpleTest, UsesBinWithLeastFreeSpace) {
//  ASSERT_ASSIGNMENTS({
//      {3000, 1},
//      {3001, 2},
//      {3000, 3},
//      { 100, 2},
//  });
//}
//
//TEST_F(PagerandoBinningSimpleTest, FreeSpaceMustBeAtLeastMinFnSize) {
//  ASSERT_ASSIGNMENTS({
//      {4095, 1},
//      {   1, 2},
//      {4095, 2},
//  });
//}
//
//TEST_F(PagerandoBinningSimpleTest, BinSizedFunctionsAlwaysGetTheirOwnBin) {
//  ASSERT_ASSIGNMENTS({
//      {4096, 1},
//      {8192, 2},
//      {   1, 3},
//  });
//}
//
//TEST_F(PagerandoBinningSimpleTest, LargeFunctionsAreStillPacked) {
//  ASSERT_ASSIGNMENTS({
//     {8000, 1},
//     { 100, 1},
//  });
//}

} // end anonymous namespace
