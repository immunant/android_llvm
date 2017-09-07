//===-- PagerandoBinning.cpp - Binning for Pagerando ----------------------===//
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

#include "llvm/CodeGen/PagerandoBinning.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include <queue>

using namespace llvm;

#define DEBUG_TYPE "pagerando"

enum class BStrat { Simple, CallGraph };
static cl::opt<BStrat> BinningStrategy(
    "pagerando-binning-strategy", cl::Hidden, cl::init(BStrat::CallGraph),
    cl::desc("Binning strategy for Pagerando"), cl::values(
        clEnumValN(BStrat::Simple, "simple", "Simple greedy strategy"),
        clEnumValN(BStrat::CallGraph, "callgraph",
                   "Put functions which call each other into the same bin")));

char PagerandoBinning::ID = 0;
INITIALIZE_PASS_BEGIN(PagerandoBinning, "pagerando-binning",
                      "Pagerando function binning", false, false)
INITIALIZE_PASS_DEPENDENCY(MachineModuleInfo);
INITIALIZE_PASS_END(PagerandoBinning, "pagerando-binning",
                    "Pagerando function binning", false, false)

ModulePass *llvm::createPagerandoBinningPass() {
  return new PagerandoBinning();
}

PagerandoBinning::PagerandoBinning() : ModulePass(ID) {
  initializePagerandoBinningPass(*PassRegistry::getPassRegistry());
}

void PagerandoBinning::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<MachineModuleInfo>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.setPreservesAll();
  ModulePass::getAnalysisUsage(AU);
}

bool PagerandoBinning::runOnModule(Module &M) {
  auto &MMI = getAnalysis<MachineModuleInfo>();

  switch (BinningStrategy.getValue()) {
  case BStrat::Simple:    return binSimple(M, MMI);
  case BStrat::CallGraph: return binCallGraph(MMI);
  }
}

unsigned PagerandoBinning::estimateFunctionSize(const MachineFunction &MF) {
  auto *TII = MF.getSubtarget().getInstrInfo();

  unsigned Size = 0;
  for (auto &MBB : MF)
    for (auto &MI : MBB)
      Size += TII->getInstSizeInBytes(MI);

  return std::max(Size, MinFnSize+0);
}

bool PagerandoBinning::binSimple(Module &M, MachineModuleInfo &MMI) {
  bool Changed = false;
  for (auto &F : M) {
    if (F.isPagerando()) {
      unsigned FnSize = estimateFunctionSize(MMI.getMachineFunction(F));
      unsigned Bin = Algo.assignToBin(FnSize);
      // Note: overwrites an existing section prefix
      F.setSectionPrefix(SectionPrefix + utostr(Bin));
      Changed = true;
    }
  }
  return Changed;
}

unsigned PagerandoBinning::Algorithm::assignToBin(unsigned FnSize) {
  unsigned Bin, FreeSpace;

  auto I = Bins.lower_bound(FnSize);
  if (I != Bins.end()) {
    std::tie(FreeSpace, Bin) = *I;
    FreeSpace -= FnSize;
    Bins.erase(I);
  } else {  // No bin with enough free space
    Bin = BinCount++;
    auto Size = FnSize % BinSize;
    FreeSpace = (Size == 0) ? 0 : (BinSize - Size);
  }

  if (FreeSpace >= MinFnSize) {
    Bins.emplace(FreeSpace, Bin);
  }

  return Bin;
}

bool PagerandoBinning::binCallGraph(MachineModuleInfo &MMI) {
  auto &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  std::map<Function*, int> NodesByFunc;
  std::map<int, Function*> FuncsByNode;
  int NodeCount = 0;

  // Create one node per SCC
  for (auto &SCC : make_range(scc_begin(&CG), scc_end(&CG))) {
    int Id = NodeCount++;
    unsigned Size = 0;
    std::set<int> Callees;

    for (auto *CGN : SCC) {
      auto *F = CGN->getFunction();
      if (!F->isPagerando()) continue;
      NodesByFunc.emplace(F, Id);
      FuncsByNode.emplace(Id, F);
      Size += estimateFunctionSize(MMI.getMachineFunction(*F));
      for (auto &CR : *CGN) {
        auto *CF = CR.second->getFunction();
        if (!CF->isPagerando()) continue;
        Callees.insert(NodesByFunc.at(CF));
        // TODO: Probably does not work since there could be cycles in call chains.
      }
    }
    CallGraph.addNode(Id, Size, Callees);
  }

  auto Assignments = CallGraph.computeBinAssignments();
  for (auto &A : Assignments) {
    int Id; unsigned Bin;
    std::tie(Id, Bin) = A;
    auto *F = FuncsByNode.at(Id);
    // Note: overwrites an existing section prefix
    F->setSectionPrefix(SectionPrefix + utostr(Bin));
  }

  return !Assignments.empty();
}

void PagerandoBinning::CallGraphAlgo::addNode(int Id, unsigned Size,
                                              std::set<int> Callees) {
  // tODO(yln) use N = Nodes[x];
  auto I = Nodes.emplace(Id, Node{Id, Size, 0, std::set<Node*>()});
  assert(I.second && "Duplicate Id");
  auto &Node = I.first->second;

  for (auto C : Callees) {
    auto &CN = Nodes.at(C);
    CN.Callers.insert(&Node);
    // TODO set own callers
    Node.TreeSize += CN.TreeSize;
  }
}

PagerandoBinning::CallGraphAlgo::Node*
PagerandoBinning::CallGraphAlgo::removeNode(std::vector<Node*> &WL) {
  std::sort(WL.begin(), WL.end(), Node::byTreeSize);
  auto I = std::upper_bound(WL.begin(), WL.end(), BinSize+0, Node::toTreeSize);
  if (I != WL.begin()) --I;
  else llvm_unreachable("no component is smaller than a page!!!!");
  auto *Node = *I;
  WL.erase(I);
  return Node;
}

void PagerandoBinning::CallGraphAlgo::adjustCallerSizes(Node *Removed) {
  std::queue<Node*> Queue({Removed});
  std::set<Node*> Discovered{Removed};

  while (!Queue.empty()) {
    auto *N = Queue.front(); Queue.pop();
    N->TreeSize -= Removed->TreeSize;
    for (auto *C : N->Callers) {
      if (Discovered.insert(C).second) {
        Queue.push(C);
      }
    }
  }
}

void PagerandoBinning::CallGraphAlgo::assignCalleesToBin(Node *Tree, unsigned Bin) {
  std::queue<Node*> Queue({Tree});
  std::set<Node*> Discovered{Tree};

  while (!Queue.empty()) {
    auto *N = Queue.front(); Queue.pop();
    N->Bin = Bin;
    for (auto *C : N->Callees) {
      if (Discovered.insert(C).second) {
        Queue.push(C);
      }
    }
  }
}

// <node id  ->  bin>
std::vector<std::pair<int, unsigned>> PagerandoBinning::CallGraphAlgo::computeBinAssignments() {
  std::vector<Node*> WorkList;
  WorkList.reserve(Nodes.size());
  for (auto &E : Nodes) {
    WorkList.push_back(&E.second);
  }

  while (!WorkList.empty()) {
    auto *Node = removeNode(WorkList);
    auto Bin = Simple.assignToBin(Node->TreeSize);
    assignCalleesToBin(Node, Bin);
    adjustCallerSizes(Node);
  }

  // TODO(yln): above loop should do this!
  std::vector<std::pair<int, unsigned>> Result;
  for (auto &E : Nodes) {
    Result.emplace_back(E.first, E.second.Bin);
  }

  return Result;
}
