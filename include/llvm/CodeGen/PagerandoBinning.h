//===-- PagerandoBinning.h - Binning for Pagerando ------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass assigns pagerando-enabled functions to bins. Normal functions
// (and pagerando wrappers) are not assigned to a bin.
// Function sizes are estimated by adding up the size of all instructions
// inside the corresponding MachineFunction. The default bin size is 4KB.
// The current bin allocation strategy is a greedy algorithm that, for every
// function, picks the bin with the smallest remaining free space that still
// accommodates the function. If such a bin does not exist, a new one is
// created. Functions that are larger than the default bin size are assigned to
// a new bin which forces the expansion of said bin.
// Because this pass estimates function sizes it should run as late as possible,
// but before Pagerando optimizer passes (since they rely on bin assignments).
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
      unsigned TreeSize;
      std::set<NodeId> Callers, Callees;

      static bool byTreeSize(const Node *A, const Node *B) {
        return A->TreeSize < B->TreeSize;
      }
      static bool toTreeSize(unsigned Size, const Node *N) {
        return Size < N->TreeSize;
      }
    };
    std::vector<Node> Nodes;
    SimpleAlgo SAlgo;

    Node *removeNode(std::vector<Node*> &WL);
    template<typename Expander, typename Action>
    void bfs(Node *Start, Expander Exp, Action Act);
    void adjustCallerSizes(Node *Removed);
    void collectCalleeAssignments(Node *Tree, Bin B, std::map<NodeId, Bin> &Agg);

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