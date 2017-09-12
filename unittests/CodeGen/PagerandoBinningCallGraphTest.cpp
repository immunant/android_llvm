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

struct PagerandoBinningCallGraphTest : public testing::Test {
  PagerandoBinning::CallGraphAlgo Algo;
  typedef unsigned NodeId;

  void defineGraph(std::initializer_list<unsigned> Sizes,
                   std::initializer_list<std::pair<NodeId, NodeId>> Edges) {
    NodeId ExpectedId = 0;
    for (auto Size : Sizes) {
      NodeId Id = Algo.addNode(Size);
      ASSERT_EQ(Id, ExpectedId++);
    }

    // Edges need to be added bottom-up; use reverse iterator.
    // Replace with std::rbegin once we have C++14
    std::vector<std::pair<NodeId, NodeId>> Vec(Edges);
    for (auto &Edge : make_range(Vec.rbegin(), Vec.rend())) {
      Algo.addEdge(Edge.first, Edge.second);
    }
  }

  void ASSERT_ASSIGNMENTS(std::initializer_list<unsigned> ExpectedBins) {
    auto Bins = Algo.computeAssignments();
    ASSERT_EQ(Bins.size(), ExpectedBins.size());

    NodeId Id = 0;
    for (auto ExpectedBin : ExpectedBins) {
      errs() << "node id: " << Id << "\n";
      unsigned Bin = Bins.at(Id++);
      ASSERT_EQ(Bin, ExpectedBin);
    }
  }
};

TEST_F(PagerandoBinningCallGraphTest, NoEdges) {
  defineGraph({2003, 2002, 2001}, {});
  ASSERT_ASSIGNMENTS({1, 1, 2});
}

TEST_F(PagerandoBinningCallGraphTest, StandardExample) {
  defineGraph(
//       0     1     2     3     4     5     6     7
      { 600,  800, 3500, 1000, 1000, 1000, 4000,  100},
      {{0, 1}, {0, 2},
       {1, 3}, {1, 4}, {1, 5},
       {2, 6}, {2, 7}
      });
// -----------------------------------------------------------------------------
//
//                     (0)                  Bin size is 4096
//                     600
//                   12000
//                      |
//            +---------+---------+
//            |                   |
//           (1)                 (2)  <-- node id
//           800                3500  <-- self size
//          3800                7600  <-- tree size
//            |                   |
//    +---------------+       +---+---+
//    |       |       |       |       |
//   (3)     (4)     (5) --> (6)     (7)
//  1000    1000    1000    4000     100
//
// -----------------------------------------------------------------------------
//
//                     (0)                  Bin (free space) -> nodes
//                     600                  1 (  96) -> 6
//                    8000
//                      |
//            +---------+---------+
//            |                   |
//       --> (1)                 (2)
//           800                3500
//          3800                3600
//            |                   |
//    +---------------+           +---+
//    |       |       |               |
//   (3)     (4)     (5)             (7)
//  1000    1000    1000             100
//
// -----------------------------------------------------------------------------
//
//                     (0)                  Bin (free space) -> nodes
//                     600                  1 (  96) -> 6
//                    4200                  2 ( 296) -> 1, 3, 4, 5
//                      |
//                      +---------+
//                                |
//                           --> (2)
//                              3500
//                              3600
//                                |
//                                +---+
//                                    |
//                                   (7)
//                                   100
//
// -----------------------------------------------------------------------------
//
//                 --> (0)                  Bin (free space) -> nodes
//                     600                  1 (  96) -> 6
//                                          2 ( 296) -> 1, 3, 4, 5
//                                          3 ( 496) -> 2, 7
//
// -----------------------------------------------------------------------------
//
//                                          Bin (free space) -> nodes
//                                          1 (  96) -> 6
//                                          2 ( 296) -> 1, 3, 4, 5
//                                          3 ( 496) -> 2, 7
//                                          4 (3496) -> 0
//
// -----------------------------------------------------------------------------
//
//                    0  1  2  3  4  5  6  7
  ASSERT_ASSIGNMENTS({4, 2, 3, 2, 2, 2, 1, 3});
}

//TEST_F(PagerandoBinningSimpleTest, UsesBinWithLeastFreeSpace) {
//  ASSERT_ASSIGNMENTS({
//      {3000, 1},
//      {3001, 2},
//      {3000, 3},
//      { 100, 2},
//  });
//}

} // end anonymous namespace
