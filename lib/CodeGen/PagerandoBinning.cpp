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
  switch (BinningStrategy.getValue()) {
  case BStrat::Simple:    return binSimple(M);
  case BStrat::CallGraph: return binCallGraph();
  }
}

unsigned PagerandoBinning::estimateFunctionSize(const Function &F) {
  auto &MF = getAnalysis<MachineModuleInfo>().getMachineFunction(F);
  auto *TII = MF.getSubtarget().getInstrInfo();

  unsigned Size = 0;
  for (auto &MBB : MF)
    for (auto &MI : MBB)
      Size += TII->getInstSizeInBytes(MI);

  return std::max(Size, MinFnSize+0);
}

bool PagerandoBinning::binSimple(Module &M) {
  bool Changed = false;
  for (auto &F : M) {
    if (F.isPagerando()) {
      unsigned FnSize = estimateFunctionSize(F);
      unsigned Bin = SAlgo.assignToBin(FnSize);
      // Note: overwrites an existing section prefix
      F.setSectionPrefix(SectionPrefix + utostr(Bin));
      Changed = true;
    }
  }
  return Changed;
}

unsigned PagerandoBinning::SimpleAlgo::assignToBin(unsigned FnSize) {
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

static bool skipFunction(const Function *F) {
  return !F || !F->isPagerando();
}

bool PagerandoBinning::binCallGraph() {
  auto &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  std::map<Function*, NodeId> FuncsToNode;

  // Create one node per SCC, if it contains at least one Pagerando function
  for (auto &SCC : make_range(scc_begin(&CG), scc_end(&CG))) {
    std::vector<Function*> Funcs;
    unsigned Size = 0;

    for (auto *CGN : SCC) {
      auto *F = CGN->getFunction();
      if (skipFunction(F)) continue;
      Funcs.push_back(F);
      Size += estimateFunctionSize(*F);
    }

    if (!Funcs.empty()) {
      NodeId Id = CGAlgo.addNode(Size);
      for (auto *F : Funcs) {
        FuncsToNode.emplace(F, Id);
      }
    }
  }

  // Add caller-callee edges
  for (auto &E : FuncsToNode) {
    Function *F; NodeId Id;
    std::tie(F, Id) = E;

    for (auto &CR : *CG[F]) {
      auto *CF = CR.second->getFunction();
      if (skipFunction(CF)) continue;
      CGAlgo.addEdge(Id, FuncsToNode.at(CF));
    }
  }

  auto Assignments = CGAlgo.computeBinAssignments();
  for (auto &E: FuncsToNode) {
    Function *F; int Id;
    std::tie(F, Id) = E;
    unsigned Bin = Assignments.at(Id);
    errs() << "node id: " << Id << "  ->  " << Bin <<"\n";
    // Note: overwrites an existing section prefix
    F->setSectionPrefix(SectionPrefix + utostr(Bin));
  }

  return !Assignments.empty();
}

PagerandoBinning::NodeId
PagerandoBinning::CallGraphAlgo::addNode(unsigned Size) {
  NodeId Id = Nodes.size();
  Nodes.emplace_back(Node{Id, Size, 0});
  return Id;
}

void PagerandoBinning::CallGraphAlgo::addEdge(NodeId Caller, NodeId Callee) {
  Node &From = Nodes.at(Caller);
  Node &To = Nodes.at(Callee);
  From.Callees.insert(&To);
  To.Callers.insert(&From);
}

std::vector<PagerandoBinning::CallGraphAlgo::Node>::iterator
PagerandoBinning::CallGraphAlgo::selectNode(std::vector<Node> &WL) {
  std::sort(WL.begin(), WL.end(), Node::byTreeSize);
  auto I = std::upper_bound(WL.begin(), WL.end(), BinSize+0, Node::toTreeSize);
  if (I != WL.begin()) --I;
  else llvm_unreachable("no component is smaller than a page!!!!");
  return I;
}

template<typename NodeT, typename SearchDirection, typename VisitAction>
static void bfs(NodeT Start, SearchDirection Expander, VisitAction Action) {
  std::queue<NodeT> Queue({Start});
  std::set<NodeT> Discovered{Start};

  while (!Queue.empty()) {
    NodeT N = Queue.front(); Queue.pop();
    Action(N);
    for (NodeT C : Expander(N)) {
      if (Discovered.insert(C).second) {
        Queue.push(C);
      }
    }
  }
}

void PagerandoBinning::CallGraphAlgo::adjustCallerSizes(Node *Removed) {
  bfs(Removed,
      [](Node *N) { return N->Callers; },
      [Removed](Node *N) { N->TreeSize -= Removed->TreeSize; });
}

void PagerandoBinning::CallGraphAlgo::collectCalleeAssignments(
    Node *Tree, unsigned TheBin, std::map<NodeId, Bin> &Agg) {
  bfs(Tree,
      [](Node *N) { return N->Callees; },
      [TheBin, &Agg](Node *N) { Agg.emplace(N->Id, TheBin); });
}

std::map<PagerandoBinning::NodeId, PagerandoBinning::Bin>
PagerandoBinning::CallGraphAlgo::computeBinAssignments() {
  std::map<NodeId, Bin> Assignments;
  while (!Nodes.empty()) {
    auto N = selectNode(Nodes);
    auto Bin = SAlgo.assignToBin(N->TreeSize);
    collectCalleeAssignments(&*N, Bin, Assignments);
    adjustCallerSizes(&*N);
    Nodes.erase(N);
  }
  return Assignments;
}
