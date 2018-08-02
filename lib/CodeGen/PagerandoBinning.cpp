//===-- PagerandoBinning.cpp - Binning for Pagerando ----------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass assigns Pagerando-enabled functions to bins. Normal functions
// (and currently also Pagerando wrappers) are not assigned to a bin. The bin
// size is 4KB.
// Function sizes are estimated by adding up the size of all instructions
// of the corresponding MachineFunction. To improve estimate accuracy this pass
// should run as late as possible, but must run before the Pagerando optimizer
// passes (since they rely on bin assignments).
//
// Binning strategies:
// -) Simple: a greedy algorithm that, for every function, picks the bin with
// the smallest remaining free space that still accommodates the function. If
// such a bin does not exist, a new one is created. Functions that are larger
// than the bin size are assigned to a new bin which forces the expansion of
// said bin.
// -) PGO: Profile-guided bin assignment. The algorithm attempts to bin together
// hot calls into the same bin by repeatedly combining hot functions with their
// caller with the hottest call site. Based on the C3 algorithm presented in
// "Optimizing Function Placement for Large-Scale Data-Center Applications,"
// Ottoni and Maher, CGO 2017.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/PagerandoBinning.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/Target/TargetMachine.h"
#include <algorithm>

using namespace llvm;

#define DEBUG_TYPE "pagerando-binning"

enum class BStrat { Simple, PGO };
static cl::opt<BStrat> BinningStrategy(
    "pagerando-binning-strategy", cl::Hidden, cl::init(BStrat::Simple),
    cl::desc("Binning strategy for Pagerando"), cl::values(
        clEnumValN(BStrat::Simple, "simple", "Simple greedy strategy"),
        clEnumValN(BStrat::PGO, "pgo", "Profile-guided strategy")));

static cl::opt<unsigned> BinSize(
    "pagerando-bin-size", cl::Hidden, cl::init(4096),
    cl::desc("Target size for pagerando bins"));


namespace {

class FirstFitAlgo {
public:
  PagerandoBinnerBase::Bin assignToBin(unsigned FnSize, unsigned getMaxBinSize, bool isUniPOT);

private:
  // <free space  ->  bin numbers>
  std::multimap<unsigned, PagerandoBinnerBase::Bin> Bins;
  unsigned BinCount = 1;
};

class SimpleBinner : public PagerandoBinnerBase {
public:
  SimpleBinner() : PagerandoBinnerBase(ID) {
    initializeSimpleBinnerPass(*PassRegistry::getPassRegistry());
  }

  static char ID; // Pass identification, replacement for typeid

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  Bin getBinAssignment(Function &F) override;

private:
  FirstFitAlgo FitAlgo;
};

class PGOBinner : public PagerandoBinnerBase {
public:
  PGOBinner() : PagerandoBinnerBase(ID) {
    initializePGOBinnerPass(*PassRegistry::getPassRegistry());
  }

  static char ID; // Pass identification, replacement for typeid

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  bool initializeBinning(Module &M) override;

  Bin getBinAssignment(Function &F) override;

private:
  struct Cluster {
    unsigned Size = 0;
    SmallVector<Function*, 10> Functions;

    // Lazily initialized during getBinAssignment phase
    PagerandoBinnerBase::Bin Bin = 0;

    Cluster(Function *F, unsigned InitialSize)
        : Size(InitialSize), Functions{F} { }

    void merge(Cluster &Other) {
      Size += Other.Size;
      Functions.insert(Functions.end(), Other.Functions.begin(), Other.Functions.end());
    }
  };

  unsigned BinCount = 1;

  FirstFitAlgo FitAlgo;

  ProfileSummaryInfo *PSI;
  bool HaveProfileInfo = false;

  using CallerWeights = DenseMap<Function*, uint64_t>;

  std::map<Function*, CallerWeights> RevCG;
  DenseMap<Function*, std::shared_ptr<Cluster>> FnToCluster;

  std::shared_ptr<Cluster> getCluster(Function *F);
  void mergeClusters(std::shared_ptr<Cluster> C1, std::shared_ptr<Cluster> C2);

  void createReverseWeightedCallGraph(CallGraph &CG);
};

} // end anonymous namespace


PagerandoBinnerBase::PagerandoBinnerBase(char &ID) : ModulePass(ID) {}

void PagerandoBinnerBase::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<MachineModuleInfo>();
  AU.addPreserved<MachineModuleInfo>();
  ModulePass::getAnalysisUsage(AU);
}

bool PagerandoBinnerBase::initializeBinning(Module &M) {
  return false;
}

bool PagerandoBinnerBase::runOnModule(Module &M) {
  bool Modified = initializeBinning(M);

  for (auto &F : M) {
    if (F.isPagerando()) {
      Bin B = getBinAssignment(F);
      setBin(F, B);
      Modified = true;
    }
  }
  return Modified;
}

void PagerandoBinnerBase::setBin(Function &F, Bin Bin) {
  // Note: overwrites an existing section prefix
  F.setSectionPrefix(SectionPrefix + utostr(Bin));
}

unsigned PagerandoBinnerBase::estimateFunctionSize(const Function &F) {
  auto &MF = *getAnalysis<MachineModuleInfo>().getMachineFunction(F);
  auto *TII = MF.getSubtarget().getInstrInfo();

  unsigned Size = 0;
  for (auto &MBB : MF)
    for (auto &MI : MBB)
      Size += TII->getInstSizeInBytes(MI);

  return std::max(Size, MinFnSize+0);
}

unsigned PagerandoBinnerBase::getMaxBinSize(const Function &F) {
  auto arch = getAnalysisIfAvailable<MachineModuleInfo>()->getMachineFunction(F)->getTarget().getTargetTriple().getArch();

  if (arch == Triple::ArchType::aarch64)
    return 511;
  else if (arch == Triple::ArchType::arm)
    return 1023; 

  // The arch is neither arm64 or arm, so just return 0
  return 0;
}

bool PagerandoBinnerBase::isUniPOT(const Function &F) {
  auto MMI = getAnalysisIfAvailable<MachineModuleInfo>();
  unsigned index = MMI->getMachineFunction(F)->getTarget().Options.MCOptions.GlobalPOTIndex;
  if (index > 0) return true;
  return false;
}

char SimpleBinner::ID = 0;
INITIALIZE_PASS_BEGIN(SimpleBinner, "pagerando-binning-simple", "Simple Function Binning",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(MachineModuleInfo)
INITIALIZE_PASS_END(SimpleBinner, "pagerando-binning-simple", "Simple Function Binning",
                    false, false)

PagerandoBinnerBase::Bin SimpleBinner::getBinAssignment(Function &F) {
  auto FnSize = estimateFunctionSize(F);
  return FitAlgo.assignToBin(FnSize, getMaxBinSize(F), isUniPOT(F));
}

void SimpleBinner::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  PagerandoBinnerBase::getAnalysisUsage(AU);
}

PagerandoBinnerBase::Bin FirstFitAlgo::assignToBin(unsigned FnSize, unsigned MaxBinSize, bool isUniPOT) {
  if (MaxBinSize > 0) {
    if (isUniPOT and (BinCount > MaxBinSize)) {
        report_fatal_error("Please increase the bin-size.");
    }
  }

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

  if (FreeSpace >= PagerandoBinnerBase::MinFnSize)
    Bins.emplace(FreeSpace, Bin);

  return Bin;
}


char PGOBinner::ID = 0;
INITIALIZE_PASS_BEGIN(PGOBinner, "pagerando-binning-pgo", "PGO Function Binning",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ProfileSummaryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(BlockFrequencyInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MachineModuleInfo)
INITIALIZE_PASS_END(PGOBinner, "pagerando-binning-pgo", "PGO Function Binning",
                    false, false)

bool PGOBinner::initializeBinning(Module &M) {
  auto &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  PSI = getAnalysis<ProfileSummaryInfoWrapperPass>().getPSI();
  if (!PSI || !PSI->hasProfileSummary()) {
    dbgs() << "No profiling information available. Falling back to simple first-fit\n";
    return false;
  }
  HaveProfileInfo = true;

  createReverseWeightedCallGraph(CG);

  std::vector<Function*> Worklist;
  for (auto &F : M) {
    if (F.isPagerando())
      Worklist.push_back(&F);
  }
  std::stable_sort(Worklist.begin(), Worklist.end(),
                   [](Function *F1, Function *F2) {
                     auto C1 = F1->getEntryCount();
                     auto C2 = F2->getEntryCount();
                     if (!C1.hasValue())
                       return false;
                     if (!C2.hasValue())
                       return true;
                     return C1.getCount() > C2.getCount();
                   });

  std::vector<std::pair<Function*, uint64_t> > WeightedCallers;
  for (auto Callee : Worklist) {
    auto &WeightMap = RevCG[Callee];
    WeightedCallers.insert(WeightedCallers.end(), WeightMap.begin(), WeightMap.end());
    std::stable_sort(WeightedCallers.begin(), WeightedCallers.end(),
                     [](std::pair<Function*, uint64_t> I1,
                        std::pair<Function*, uint64_t> I2) {
                       return I1.second > I2.second;
                     });

    // Select a cluster to merge into
    auto CalleeCluster = getCluster(Callee);
    for (auto I : WeightedCallers) {
      Function *Caller = I.first;
      auto CallerCluster = getCluster(Caller);
      if (CallerCluster->Size + CalleeCluster->Size <= BinSize) {
        mergeClusters(CallerCluster, CalleeCluster);
        break;
      }
    }

    WeightedCallers.clear();
  }

  return false;
}

void PGOBinner::createReverseWeightedCallGraph(CallGraph &CG) {
  for (auto &CGI : CG) {
    Function *Caller = CGI.second->getFunction();
    if (!Caller) // External node
      continue;
    if (!Caller->isPagerando())
      continue;
    CallerWeights &Weights = RevCG[Caller];
    auto &BFI = getAnalysis<BlockFrequencyInfoWrapperPass>(*Caller).getBFI();
    for (auto &CR : *CGI.second) {
      Instruction *CallInst = cast<Instruction>(CR.first);
      Function *Callee = CR.second->getFunction();
      if (!Callee) // External node
        continue;
      if (!Callee->isPagerando())
        continue;
      auto Count = PSI->getProfileCount(CallInst, &BFI);
      if (Count)
        Weights[Callee] += *Count;
    }
  }
}

std::shared_ptr<PGOBinner::Cluster> PGOBinner::getCluster(Function *F) {
  auto I = FnToCluster.find(F);
  if (I != FnToCluster.end())
    return I->second;

  auto C = std::make_shared<Cluster>(F, estimateFunctionSize(*F));
  auto Insert = FnToCluster.try_emplace(F, C);
  return C;
}

void PGOBinner::mergeClusters(std::shared_ptr<Cluster> C1, std::shared_ptr<Cluster> C2) {
  C1->merge(*C2);
  for (auto *F : C2->Functions)
    FnToCluster[F] = C1;
}

PagerandoBinnerBase::Bin PGOBinner::getBinAssignment(Function &F) {
  if (!HaveProfileInfo) {
    // Fall back to simple binning
    auto FnSize = estimateFunctionSize(F);
    return FitAlgo.assignToBin(FnSize, getMaxBinSize(F), isUniPOT(F));
  }

  auto Cluster = FnToCluster[&F];
  if (Cluster->Bin == 0) {
    Cluster->Bin = FitAlgo.assignToBin(Cluster->Size, getMaxBinSize(F), isUniPOT(F));
  }
  return Cluster->Bin;
}

void PGOBinner::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<ProfileSummaryInfoWrapperPass>();
  AU.addRequired<BlockFrequencyInfoWrapperPass>();
  AU.setPreservesAll();
  PagerandoBinnerBase::getAnalysisUsage(AU);
}


ModulePass *llvm::createPagerandoBinningPass() {
  switch (BinningStrategy.getValue()) {
  case BStrat::Simple:    return new SimpleBinner();
  case BStrat::PGO:       return new PGOBinner();
  }
  llvm_unreachable("Unexpected binning strategy");
}
