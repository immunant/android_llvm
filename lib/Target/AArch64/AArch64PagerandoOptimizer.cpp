//===-- AArch64PagerandoOptimizer.cpp - Optimizes intra-bin function calls ===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass optimizes calls inside the same position-independent bin to direct
// calls to avoid the overhead of indirect calls through the POT.
//
//===----------------------------------------------------------------------===//

#include "AArch64.h"
#include "AArch64MachineFunctionInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetLowering.h"

using namespace llvm;

#define DEBUG_TYPE "pagerando"

namespace {
class AArch64PagerandoOptimizer : public MachineFunctionPass {
public:
  static char ID;
  explicit AArch64PagerandoOptimizer() : MachineFunctionPass(ID) {
    initializeAArch64PagerandoOptimizerPass(*PassRegistry::getPassRegistry());
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::TracksLiveness);
  }

private:
  void createDirectCall(MachineInstr *MI, const Function *Callee);
  void optimizeCalls(MachineInstr *MI, const Function *Callee);
};
} // end anonymous namespace

char AArch64PagerandoOptimizer::ID = 0;
INITIALIZE_PASS(AArch64PagerandoOptimizer, "pagerando-optimizer-aarch64",
                "Pagerando intra-bin optimizer for AArch64", false, false)

FunctionPass *llvm::createAArch64PagerandoOptimizerPass() {
  return new AArch64PagerandoOptimizer();
}

static const Function *getCallee(const MachineInstr &MI) {
  return cast<Function>(MI.getOperand(2).getGlobal());
}

static bool isIntraBin(const MachineInstr &MI, StringRef BinPrefix) {
  return MI.getOpcode() == AArch64::MOVaddrBIN
      && getCallee(MI)->getSectionPrefix() == BinPrefix;
}

bool AArch64PagerandoOptimizer::runOnMachineFunction(MachineFunction &MF) {
  auto &F = *MF.getFunction();
  // This pass is an optimization (optional), therefore check skipFunction
  if (!F.isPagerando() || skipFunction(F)) {
    return false;
  }

  // Section prefix is assigned by PagerandoBinning pass
  auto BinPrefix = F.getSectionPrefix().getValue();

  // Collect intra-bin references
  std::vector<MachineInstr*> Worklist;
  for (auto &BB : MF) {
    for (auto &MI : BB) {
      if (isIntraBin(MI, BinPrefix)) {
        Worklist.push_back(&MI);
      }
    }
  }

  if (Worklist.empty()) {
    return false;
  }

  // Optimize intra-bin calls
  for (auto *MI : Worklist) {
    auto *Callee = getCallee(*MI);
    optimizeCalls(MI, Callee);
  }

  return true;
}

void AArch64PagerandoOptimizer::optimizeCalls(MachineInstr *MI,
                                              const Function *Callee) {
  auto &MRI = MI->getParent()->getParent()->getRegInfo();

  SmallVector<MachineInstr*, 4> Queue{MI};

  while (!Queue.empty()) {
    MI = Queue.pop_back_val();

    if (!MI->isCall()) { // Not a call, enqueue users
      for (auto &Op : MI->defs()) {
        for (auto &User : MRI.use_instructions(Op.getReg())) {
          Queue.push_back(&User);
        }
      }
    } else {
      createDirectCall(MI, Callee);
    }
    MI->eraseFromParent();
    // Note: this might be the only use of the preceding AArch64::LOADpot pseudo
    // instruction. We schedule the DeadMachineInstructionElim pass after this
    // pass to get rid of it.
  }
}

// TODO(yln): compile all of AOSP, then inline.
static unsigned toDirectCall(unsigned Opc) {
  assert(Opc == AArch64::BLR);
  return AArch64::BL;
}

void AArch64PagerandoOptimizer::createDirectCall(MachineInstr *MI,
                                                 const Function *Callee) {
  auto &MBB = *MI->getParent();
  auto &TII = *MBB.getParent()->getSubtarget().getInstrInfo();

  // TODO(yln): remove
  if (MI->getOpcode() != AArch64::BLR) {
    errs() << MI << "\n";
    std::exit(-7);
  }
  auto Opc = toDirectCall(MI->getOpcode());
  auto MIB = BuildMI(MBB, *MI, MI->getDebugLoc(), TII.get(Opc))
      .addGlobalAddress(Callee);

  // Copy over remaining operands
  auto RemainingOps = make_range(MI->operands_begin() + 1, MI->operands_end());
  for (auto &Op : RemainingOps) {
    MIB.add(Op);
  }
}
