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
  unsigned Number;
  unsigned FreeSpace;

  static const unsigned BinSize;

  Bin(unsigned number) : Number(number), FreeSpace(BinSize) { }
};

const unsigned Bin::BinSize = 4096;

class PagerandoBinning : public ModulePass {
  // Map from free space -> Bin
  typedef std::multimap<unsigned, Bin> BinMap;
  BinMap Bins;
  unsigned BinCount;

public:
  static char ID; // Pass identification, replacement for typeid
  PagerandoBinning() : ModulePass(ID), BinCount(1) {
    initializePagerandoBinningPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<MachineModuleInfo>();
    AU.addPreserved<MachineModuleInfo>();
    AU.setPreservesAll();
    ModulePass::getAnalysisUsage(AU);
  }
};
}

char PagerandoBinning::ID = 0;

namespace llvm {
ModulePass *createPagerandoBinningPass() { return new PagerandoBinning(); }
}

INITIALIZE_PASS_BEGIN(PagerandoBinning, "pagerando-binning",
                      "Pagerando binning", false, false)
INITIALIZE_PASS_DEPENDENCY(MachineModuleInfo);
INITIALIZE_PASS_END(PagerandoBinning, "pagerando-binning",
                    "Pagerando binning", false, false)

static unsigned GetFunctionSizeInBytes(const MachineFunction &MF, const TargetInstrInfo *TII) {
  unsigned FnSize = 0;
  for (auto &MBB : MF)
    for (auto &MI : MBB)
      FnSize += TII->getInstSizeInBytes(MI);
  return FnSize;
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

    MachineFunction &MF = MMI.getMachineFunction(F);
    const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();

    unsigned FnSize = GetFunctionSizeInBytes(MF, TII);

    auto I = Bins.lower_bound(FnSize);
    if (I == Bins.end())
      I = Bins.emplace(Bin::BinSize, BinCount++);

    // Add the function to the given bin
    DEBUG(dbgs() << "Putting function '" << MF.getName() << "' with size " << FnSize << " in bin " << I->second.Number << " with free space " << I->second.FreeSpace << '\n');
    MMI.setBin(&F, I->second.Number);
    if (FnSize <= I->second.FreeSpace)
      I->second.FreeSpace -= FnSize;
    else
      I->second.FreeSpace = (FnSize - I->second.FreeSpace) % Bin::BinSize;
    Bins.insert(std::make_pair(I->second.FreeSpace, I->second));
    Bins.erase(I);
  }

  return true;
}

