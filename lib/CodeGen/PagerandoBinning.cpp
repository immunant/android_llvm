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
  static constexpr unsigned BinSize = 4096;
  // Map <free space -> bin numbers>
  typedef std::multimap<unsigned, unsigned> BinMap;
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

  // Assign all functions to a bin
  for (auto &F : M) {
    if (!F.isRandPage()) {
      // Put all normal functions (and wrappers) into bin 0.
      MMI.setBin(&F, 0);
      continue;
    }

    unsigned FnSize = ComputeFunctionSize(F, MMI);
    unsigned Bin, FreeSpace;

    auto I = Bins.lower_bound(FnSize);
    if (I == Bins.end()) {  // No bin with enough free space
      Bin = BinCount++;
      FreeSpace = BinSize;
    } else {                // Found eligible bin
      Bin = I->second;
      FreeSpace = I->first;
      Bins.erase(I);
    }

    DEBUG(dbgs() << "Putting function '" << F.getName()
                 << "' with size " << FnSize
                 << " in bin " << Bin
                 << " with free space " << FreeSpace << '\n');

    MMI.setBin(&F, Bin);

    // Update <free space -> bin numbers> mapping
    if (FnSize <= FreeSpace) {
      FreeSpace -= FnSize;
    } else {
      // TODO(yln): I don't think this works
      FreeSpace = (FnSize - FreeSpace) % BinSize;
    }
    Bins.emplace(FreeSpace, Bin);
  }

  return true;
}
