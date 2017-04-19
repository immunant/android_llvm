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

  SmallVector<int, 8> PGLTOffCPEntries;
  SmallVector<int, 8> BinOffCPEntries;

  auto &CPEntries = ConstantPool->getConstants();

  // Find all constant pool entries referencing PGLT-indirect symbols in the
  // same bin
  for (int i = 0, e = CPEntries.size(); i < e; ++i) {
    auto &Entry = CPEntries[i];
    if (!Entry.isMachineConstantPoolEntry())
      continue;

    ARMConstantPoolValue *ACPV =
      static_cast<ARMConstantPoolValue*>(Entry.Val.MachineCPVal);
    if (ACPV->getModifier() == ARMCP::PGLTOFF) {
      const GlobalValue *GV = cast<ARMConstantPoolConstant>(ACPV)->getGV();
      if (isSameBin(GV)) 
        PGLTOffCPEntries.push_back(i);
    } else if (ACPV->getModifier() == ARMCP::BINOFF) {
      const GlobalValue *GV = cast<ARMConstantPoolConstant>(ACPV)->getGV();
      if (isSameBin(GV)) 
        BinOffCPEntries.push_back(i);
    }
  }

  // Replace users of PGLT-indirect CP entries with direct calls
  replacePGLTUses(PGLTOffCPEntries);

  // Delete unneeded CP entries

  return !PGLTOffCPEntries.empty();
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

    unsigned DestReg = MI->getOperand(0).getReg();
    SmallVector<MachineInstr*, 4> InstrQueue;
    for (auto &User : MRI.use_instructions(DestReg))
      InstrQueue.push_back(&User);

    while (!InstrQueue.empty()) {
      MachineInstr *User = InstrQueue.back();
      InstrQueue.pop_back();
      if (User->isCall()) {
        unsigned CallOpc = isThumb2 ? ARM::tBL : ARM::BL;
        MachineInstrBuilder MIB = BuildMI(*User->getParent(), *User,
                                          User->getDebugLoc(), TII->get(CallOpc));
        if (isThumb2)
          MIB.add(predOps(ARMCC::AL));
        MIB.addGlobalAddress(GV, 0, 0);
        for (int i = 1, e = User->getNumOperands(); i < e; ++i)
          MIB.add(User->getOperand(i));
      } else {
        for (auto Op : User->defs()) {
          for (auto &User : MRI.use_instructions(Op.getReg()))
            InstrQueue.push_back(&User);
        }
      }
    }
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
