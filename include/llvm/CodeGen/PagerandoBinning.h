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

namespace llvm {

class PagerandoBinnerBase : public ModulePass {
public:
  explicit PagerandoBinnerBase(char &ID);

  virtual void getAnalysisUsage(AnalysisUsage &AU) const override;

  typedef unsigned Bin;
  static constexpr unsigned MinFnSize = 2;  // 'bx lr' on ARM thumb
  static constexpr auto SectionPrefix = ".bin_";

  bool runOnModule(Module &M) override;

protected:
  virtual bool initializeBinning(Module &M);
  virtual Bin getBinAssignment(Function &F) = 0;

  bool isUniPOT();
  unsigned maxBinSize();
  unsigned estimateFunctionSize(const Function &F);
private:
  static void setBin(Function &F, Bin Bin);
};

} // end namespace llvm

#endif
