//===-- PagerandoBinning.cpp - Binning for pagerando ---------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
// Copyright 2016, 2017 Immunant, Inc.
//
//===----------------------------------------------------------------------===//
//
//
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include "llvm/Target/TargetInstrInfo.h"

using namespace llvm;

#define DEBUG_TYPE "pagerando"

namespace {
struct Bin {
  static constexpr unsigned Size = 4096;

  unsigned Number;
  unsigned FreeSpace;

  Bin(unsigned Number) : Number(Number), FreeSpace(Size) { }
};

class PagerandoBinning : public ModulePass {
public:
  static char ID;
  explicit PagerandoBinning() : ModulePass(ID), BinCount(1) {
    initializePagerandoBinningPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    ModulePass::getAnalysisUsage(AU);
    AU.addRequired<MachineModuleInfo>();
    AU.addPreserved<MachineModuleInfo>();
    AU.setPreservesAll();
  }

private:
  // Map from free space -> Bin
  typedef std::multimap<unsigned, Bin> BinMap;
  BinMap Bins;
  unsigned BinCount;
};
} // end anonymous namespace

char PagerandoBinning::ID = 0;
INITIALIZE_PASS_BEGIN(PagerandoBinning, "pagerando-binning",
                      "Pagerando binning", false, false)
INITIALIZE_PASS_DEPENDENCY(MachineModuleInfo);
INITIALIZE_PASS_END(PagerandoBinning, "pagerando-binning",
                    "Pagerando binning", false, false)

ModulePass *llvm::createPagerandoBinningPass() {
  return new PagerandoBinning();
}

static unsigned ComputeFunctionSize(const Function &F, MachineModuleInfo &MMI) {
  const MachineFunction &MF = MMI.getMachineFunction(F);
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();

  unsigned Size = 0;
  for (auto &MBB : MF)
    for (auto &MI : MBB)
      Size += TII->getInstSizeInBytes(MI);

  return Size;
}

bool PagerandoBinning::runOnModule(Module &M) {
  MachineModuleInfo &MMI = getAnalysis<MachineModuleInfo>();

  // Bin all functions
  for (auto &F : M) {
    if (!F.isRandPage()) {
      // Put all normal functions (and wrappers) into bin 0.
      MMI.setBin(&F, 0);
      continue;
    }

    unsigned FnSize = ComputeFunctionSize(F, MMI);

    auto I = Bins.lower_bound(FnSize);
    if (I == Bins.end())
      I = Bins.emplace(Bin::Size, BinCount++);

    // Add the function to the given bin
    DEBUG(dbgs() << "Putting function '" << F.getName() << "' with size " << FnSize << " in bin " << I->second.Number << " with free space " << I->second.FreeSpace << '\n');
    MMI.setBin(&F, I->second.Number);
    if (FnSize <= I->second.FreeSpace)
      I->second.FreeSpace -= FnSize;
    else
      I->second.FreeSpace = (FnSize - I->second.FreeSpace) % Bin::Size;
    Bins.insert(std::make_pair(I->second.FreeSpace, I->second));
    Bins.erase(I);
  }

  return true;
}
