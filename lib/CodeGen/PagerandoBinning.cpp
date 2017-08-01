//===-- PagerandoBinning.cpp - Pagerando binning pass ---------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
// Copyright 2016, 2017 Immunant, Inc.
//
//===----------------------------------------------------------------------===//
//
// This pass assigns pagerando-enabled functions to bins. Normal functions
// (and pagerando wrappers) are put into to the default bin #0.
// Function sizes are estimated by adding up the size of all instructions
// inside the corresponding MachineFunction. The default bin size is 4KB.
// The current bin allocation strategy is a greedy algorithm that, for every
// function, picks the bin with the smallest remaining free space that still
// accommodates the function. If such a bin does not exist, a new one is
// created. Functions that are larger than the default bin size are assigned to
// a new bin which forces the expansion of said bin.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/PagerandoBinning.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetSubtargetInfo.h"

using namespace llvm;

#define DEBUG_TYPE "pagerando"

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
  AU.setPreservesAll();
  ModulePass::getAnalysisUsage(AU);
}

bool PagerandoBinning::runOnModule(Module &M) {
  auto &MMI = getAnalysis<MachineModuleInfo>();

  for (auto &F : M) {
    auto &MF = MMI.getMachineFunction(F);
    if (F.isPagerando()) {
      unsigned Bin = AssignToBin(MF);
      // Note: overwrites an existing section prefix
      F.setSectionPrefix(SectionPrefix + utostr(Bin));
    }
  }

  return true;
}

unsigned PagerandoBinning::AssignToBin(const MachineFunction &MF) {
  unsigned FnSize = ComputeFunctionSize(MF);
  unsigned Bin, FreeSpace;

  auto I = Bins.lower_bound(FnSize);
  if (I == Bins.end()) {  // No bin with enough free space
    Bin = BinCount++;
    if (FnSize % BinSize == 0) { // Function size is a multiple of bin size
      FreeSpace = 0;
    } else {
      FreeSpace = BinSize - (FnSize % BinSize);
    }
  } else {                // Found eligible bin
    Bin = I->second;
    FreeSpace = I->first - FnSize;
    Bins.erase(I);
  }

  if (FreeSpace >= MinFnSize) {
    Bins.emplace(FreeSpace, Bin);
  }

  DEBUG(dbgs() << "Assigning function '" << MF.getName()
               << "' with size " << FnSize
               << " to bin #" << Bin
               << " with remaining free space " << FreeSpace << '\n');

  return Bin;
}

unsigned PagerandoBinning::ComputeFunctionSize(const MachineFunction &MF) {
  auto *TII = MF.getSubtarget().getInstrInfo();

  unsigned Size = 0;
  for (auto &MBB : MF)
    for (auto &MI : MBB)
      Size += TII->getInstSizeInBytes(MI);

  assert(Size >= MinFnSize);
  return Size;
}
