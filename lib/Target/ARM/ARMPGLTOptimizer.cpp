//===-- ARMLoadStoreOptimizer.cpp - ARM load / store opt. pass ------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file This pass optimizes calls inside the same position-independent bin to
/// direct calls to avoid the overhead of indirect calls through the PGLT.
//
//===----------------------------------------------------------------------===//

#include "ARM.h"
#include "ARMBaseInstrInfo.h"
#include "ARMConstantPoolValue.h"
#include "ARMMachineFunctionInfo.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
using namespace llvm;

#define DEBUG_TYPE "arm-pglt-opt"

#define ARM_PGLT_OPT_NAME "ARM PGLT interwork optimization pass"

namespace {
  struct ARMPGLTOpt : public MachineFunctionPass {
    static char ID;
    ARMPGLTOpt() : MachineFunctionPass(ID) {}

    bool runOnMachineFunction(MachineFunction &Fn) override;

    MachineFunctionProperties getRequiredProperties() const override {
      return MachineFunctionProperties().set(
          MachineFunctionProperties::Property::TracksLiveness);
    }

    StringRef getPassName() const override { return ARM_PGLT_OPT_NAME; }

  private:
    MachineFunction *MF;
    const MachineModuleInfo *MMI;
    const TargetInstrInfo *TII;
    unsigned CurBin;
    MachineConstantPool *ConstantPool;
    bool isThumb2;
    
    bool isSameBin(const GlobalValue *GV);
    void replacePGLTUses(SmallVectorImpl<int> &CPEntries);
    void deleteOldCPEntries(SmallVectorImpl<int> &CPEntries);
  };
  char ARMPGLTOpt::ID = 0;
}

INITIALIZE_PASS(ARMPGLTOpt, "arm-pglt-opt", ARM_PGLT_OPT_NAME, false,
                false)

bool ARMPGLTOpt::runOnMachineFunction(MachineFunction &Fn) {
  if (!Fn.getFunction()->isRandPage() || skipFunction(*Fn.getFunction()))
    return false;

  MF = &Fn;
  MMI = &Fn.getMMI();
  TII = Fn.getSubtarget().getInstrInfo();
  CurBin = MMI->getBin(Fn.getFunction());
  ConstantPool = Fn.getConstantPool();
  isThumb2 = Fn.getInfo<ARMFunctionInfo>()->isThumb2Function();

  SmallVector<int, 8> PGLTCPEntries;

  auto &CPEntries = ConstantPool->getConstants();

  // Find all constant pool entries referencing PGLT-indirect symbols in the
  // same bin
  for (int i = 0, e = CPEntries.size(); i < e; ++i) {
    auto &Entry = CPEntries[i];
    if (!Entry.isMachineConstantPoolEntry())
      continue;

    ARMConstantPoolValue *ACPV =
      static_cast<ARMConstantPoolValue*>(Entry.Val.MachineCPVal);
    if (ACPV->getModifier() == ARMCP::PGLTOFF ||
        ACPV->getModifier() == ARMCP::BINOFF) {
      const GlobalValue *GV = cast<ARMConstantPoolConstant>(ACPV)->getGV();
      if (isSameBin(GV)) 
        PGLTCPEntries.push_back(i);
    }
  }

  if (PGLTCPEntries.empty())
    return false;

  // Replace users of PGLT-indirect CP entries with direct calls
  replacePGLTUses(PGLTCPEntries);

  // Delete unneeded CP entries
  deleteOldCPEntries(PGLTCPEntries);

  return true;
}

void ARMPGLTOpt::replacePGLTUses(SmallVectorImpl<int> &CPEntries) {
  MachineRegisterInfo &MRI = MF->getRegInfo();

  std::vector<std::pair<MachineInstr*, const GlobalValue*> > UsesToReplace;
  for (auto &BB : *MF) {
    for (auto &MI : BB) {
      if (MI.mayLoad() && MI.getOperand(1).isCPI()) {
        int CPIndex = MI.getOperand(1).getIndex();

        const ARMConstantPoolConstant *ACPC = nullptr;
        for (int i : CPEntries) {
          if (i == CPIndex) {
            const MachineConstantPoolEntry &Entry = ConstantPool->getConstants()[i];
            ACPC = static_cast<const ARMConstantPoolConstant*>(Entry.Val.MachineCPVal);
            break;
          }
        }

        if (ACPC)
          UsesToReplace.emplace_back(&MI, ACPC->getGV());
      }
    }
  }

  for (auto I : UsesToReplace) {
    MachineInstr *MI = I.first;
    const GlobalValue *GV = I.second;

    SmallVector<MachineInstr*, 4> InstrQueue;
    InstrQueue.push_back(MI);

    while (!InstrQueue.empty()) {
      MachineInstr *User = InstrQueue.back();
      InstrQueue.pop_back();
      if (User->isCall()) {
        unsigned CallOpc;
        switch (User->getOpcode()) {
        case ARM::TCRETURNri:
          CallOpc = ARM::TCRETURNdi;
          break;
        case ARM::BLX:
          CallOpc = ARM::BL;
          break;
        case ARM::tBLXr:
          CallOpc = ARM::tBL;
          break;
        default:
          llvm_unreachable("Unhandled ARM call opcode.");
        }
        MachineInstrBuilder MIB = BuildMI(*User->getParent(), *User,
                                          User->getDebugLoc(), TII->get(CallOpc));
        if (isThumb2)
          MIB.add(predOps(ARMCC::AL));
        MIB.addGlobalAddress(GV, 0, 0);
        for (int i = 1, e = User->getNumOperands(); i < e; ++i)
          MIB.add(User->getOperand(i));
        User->eraseFromParent();
      } else {
        for (auto Op : User->defs()) {
          for (auto &User : MRI.use_instructions(Op.getReg()))
            InstrQueue.push_back(&User);
        }
        User->eraseFromParent();
      }
    }
  }
}

void ARMPGLTOpt::deleteOldCPEntries(SmallVectorImpl<int> &CPEntries) {
  std::sort(CPEntries.begin(), CPEntries.end());
  std::vector<int> IndexMapping;
  int NewI = 0;
  for (int i = 0, e = ConstantPool->getConstants().size(); i < e; ++i) {
    bool deleted = false;
    for (auto DeleteI : CPEntries) {
      if (i == DeleteI) {
        deleted = true;
        break;
      }
    }
    if (deleted)
      IndexMapping.push_back(-1);
    else
      IndexMapping.push_back(NewI++);
  }

  for (int i = 0, e = IndexMapping.size(); i < e; ++i) {
    DEBUG(dbgs() << "Index mapping " << i << " -> " << IndexMapping[i] << '\n');
  }

  for (auto &BB : *MF) {
    for (auto &MI : BB) {
      for (auto &Op : MI.explicit_uses())
        if (Op.isCPI()) {
          int CPIndex = Op.getIndex();
          assert (IndexMapping[CPIndex] != -1 &&
                  "We should already have deleted this constant pool use");

          Op.setIndex(IndexMapping[CPIndex]);
        }
    }
  }

  // Iterate entries to delete in reverse order so each deletion will not affect
  // the index of future deletions.
  for (auto I = CPEntries.rbegin(), E = CPEntries.rend(); I != E; ++I) {
    ConstantPool->eraseIndex(*I);
  }
}

bool ARMPGLTOpt::isSameBin(const GlobalValue *GV) {
  if (auto *F = dyn_cast<Function>(GV))
    return MMI->getBin(F) == CurBin;

  return false;
}

/// Returns an instance of the load / store optimization pass.
FunctionPass *llvm::createARMPGLTOptimizationPass() {
  return new ARMPGLTOpt();
}
