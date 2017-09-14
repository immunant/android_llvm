//===-- PagerandoBinning.h - Binning for Pagerando ------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass assigns Pagerando-enabled functions to bins. See the implementation
// file for a description of the algorithm.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_PAGERANDOBINNING_H
#define LLVM_CODEGEN_PAGERANDOBINNING_H

#include "llvm/Pass.h"
#include <map>
#include <set>
#include <vector>

namespace llvm {

class PagerandoBinning : public ModulePass {
public:
  static char ID;
  static constexpr auto SectionPrefix = ".bin_";

  explicit PagerandoBinning();

  void getAnalysisUsage(AnalysisUsage &AU) const override;
  bool runOnModule(Module &M) override;

private:
  static constexpr unsigned BinSize = 4096; // one page
  static constexpr unsigned MinFnSize = 2;  // 'bx lr' on ARM thumb

  typedef unsigned Bin;
  typedef unsigned NodeId;

  static void setBin(Function &F, Bin Bin);

  unsigned estimateFunctionSize(const Function &F);
  bool binSimple(Module &M);
  bool binCallGraph();

public:
  class SimpleAlgo {
    std::multimap<unsigned, Bin> Bins; // <free space  ->  bin numbers>
    unsigned BinCount = 1;
  public:
    Bin assignToBin(unsigned FnSize);
  };

  class CallGraphAlgo {
    struct Node {
      NodeId Id;
      unsigned Size, TraSize; // Transitive size
      std::set<NodeId> Callers, TraCallees;

      static bool byTransitiveSize(const Node *A, const Node *B) {
        return A->TraSize < B->TraSize;
      }
      static bool toTransitiveSize(unsigned Size, const Node *N) {
        return Size < N->TraSize;
      }
    };
    std::vector<Node> Nodes;
    SimpleAlgo SAlgo;

    void computeTransitiveSize(Node &N);
    Node *selectNode(std::vector<Node*> &WL);
    void assignAndRemoveCallees(Node &N, Bin B, std::map<NodeId, Bin> &Bins,
                                std::vector<Node*> &WL);
    void adjustCallerSizes(Node *Start);

  public:
    NodeId addNode(unsigned Size);
    void addEdge(NodeId Caller, NodeId Callee);
    std::map<NodeId, Bin> computeAssignments();
  };

private:
  SimpleAlgo SAlgo;
  CallGraphAlgo CGAlgo;
};

} // end namespace llvm

#endif