//===-- PagerandoBinning.h - Pagerando binning pass -----------------------===//
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

#ifndef LLVM_CODEGEN_PAGERANDOBINNING_H
#define LLVM_CODEGEN_PAGERANDOBINNING_H

#include "llvm/Pass.h"
#include <map>

namespace llvm {
class MachineFunction;

class PagerandoBinning : public ModulePass {
public:
  static char ID;
  static constexpr auto SectionPrefix = ".bin_";

  explicit PagerandoBinning();

  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  static constexpr unsigned BinSize = 4096; // one page
  static constexpr unsigned MinFnSize = 2;  // 'bx lr' on ARM thumb

  // Map <free space -> bin number>
  std::multimap<unsigned, unsigned> Bins;
  unsigned BinCount = 1;

  unsigned AssignToBin(const MachineFunction &MF);
  unsigned ComputeFunctionSize(const MachineFunction &MF);
};

} // end namespace llvm

#endif