//===-- ARMPOTOptimizer.cpp - ARM load / store opt. pass ------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file This pass optimizes calls inside the same position-independent bin to
/// direct calls to avoid the overhead of indirect calls through the POT.
//
//===----------------------------------------------------------------------===//

#include "ARM.h"
#include "ARMBaseInstrInfo.h"
#include "ARMBaseRegisterInfo.h"
#include "ARMConstantPoolValue.h"
#include "ARMMachineFunctionInfo.h"
#include "ARMSubtarget.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"

using namespace llvm;

#define DEBUG_TYPE "pagerando"

#define ARM_POT_OPT_NAME "ARM POT interwork optimization pass"

namespace {
class ARMPOTOpt : public MachineFunctionPass {
public:
  static char ID;
  explicit ARMPOTOpt() : MachineFunctionPass(ID) {}
  // TODO(yln): why not self-registering

  bool runOnMachineFunction(MachineFunction &Fn) override;

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::TracksLiveness);
  }

  StringRef getPassName() const override { return ARM_POT_OPT_NAME; }

private:
  MachineFunction *MF;
  const MachineModuleInfo *MMI;
  const TargetInstrInfo *TII;
  const TargetLowering *TLI;
  ARMFunctionInfo *AFI;
  const ARMSubtarget *Subtarget;
  StringRef CurBinPrefix;
  MachineConstantPool *ConstantPool;
  bool isThumb2;

  bool isSameBin(const GlobalValue *GV);
  void replacePOTUses(SmallVectorImpl<int> &CPEntries);
  void deleteOldCPEntries(SmallVectorImpl<int> &CPEntries);
};
} // end anonymous namespace

char ARMPOTOpt::ID = 0;
INITIALIZE_PASS(ARMPOTOpt, "pagerando-optimizer",
                "Pagerando intra-bin optimizer", false, false)

FunctionPass *llvm::createARMPOTOptimizationPass() {
  return new ARMPOTOpt();
}

bool ARMPOTOpt::runOnMachineFunction(MachineFunction &Fn) {
  if (!Fn.getFunction()->isPagerando() || skipFunction(*Fn.getFunction()))
    return false;

  MF = &Fn;
  MMI = &Fn.getMMI();
  Subtarget = &static_cast<const ARMSubtarget &>(Fn.getSubtarget());
  TII = Subtarget->getInstrInfo();
  TLI = Subtarget->getTargetLowering();
  AFI = Fn.getInfo<ARMFunctionInfo>();
  // If we are in a RandPage, it should always have a section prefix
  CurBinPrefix = Fn.getFunction()->getSectionPrefix().getValue();
  ConstantPool = Fn.getConstantPool();
  isThumb2 = Fn.getInfo<ARMFunctionInfo>()->isThumb2Function();

  SmallVector<int, 8> POTCPEntries;

  auto &CPEntries = ConstantPool->getConstants();

  // Find all constant pool entries referencing POT-indirect symbols in the
  // same bin
  for (int i = 0, e = CPEntries.size(); i < e; ++i) {
    auto &Entry = CPEntries[i];
    if (!Entry.isMachineConstantPoolEntry())
      continue;

    ARMConstantPoolValue *ACPV =
      static_cast<ARMConstantPoolValue*>(Entry.Val.MachineCPVal);
    if (ACPV->getModifier() == ARMCP::POTOFF ||
        ACPV->getModifier() == ARMCP::BINOFF) {
      const GlobalValue *GV = cast<ARMConstantPoolConstant>(ACPV)->getGV();
      if (isSameBin(GV)) 
        POTCPEntries.push_back(i);
    }
  }

  if (POTCPEntries.empty())
    return false;

  // Replace users of POT-indirect CP entries with direct calls
  replacePOTUses(POTCPEntries);

  // Delete unneeded CP entries
  deleteOldCPEntries(POTCPEntries);

  return true;
}

static bool IsIndirectCall(unsigned Opc) {
  return Opc == ARM::BX_CALL || Opc == ARM::tBX_CALL;
}

static unsigned NormalizeCallOpcode(unsigned Opc) {
  switch (Opc) {
  case ARM::TCRETURNri: return ARM::TCRETURNdi;
  case ARM::BLX:        return ARM::BL;
  case ARM::tBLXr:      return ARM::tBL;
  case ARM::BX_CALL:    return Opc;
  case ARM::tBX_CALL:   return Opc;
  default:
    llvm_unreachable("Unhandled ARM call opcode.");
  }
}

void ARMPOTOpt::replacePOTUses(SmallVectorImpl<int> &CPEntries) {
  MachineRegisterInfo &MRI = MF->getRegInfo();

  std::vector<std::pair<MachineInstr*, const GlobalValue*> > UsesToReplace;
  for (auto &BB : *MF) {
    for (auto &MI : BB) {
      if (MI.mayLoad() && MI.getNumOperands() > 1 && MI.getOperand(1).isCPI()) {
        int CPIndex = MI.getOperand(1).getIndex();

        const ARMConstantPoolConstant *ACPC = nullptr;
        for (int i : CPEntries) {
          if (i == CPIndex) {
            const MachineConstantPoolEntry &Entry = ConstantPool->getConstants()[i];
            // TODO(yln): should the cast below be a dynamic_cast?
            // If not, then ACPC should probably defined here and there is not reason
            // UsesToReplace.emplace_back needs to be done outside the loop.
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

      if (!User->isCall()) {
        for (auto Op : User->defs()) {
          for (auto &User : MRI.use_instructions(Op.getReg()))
            InstrQueue.push_back(&User);
        }
        User->eraseFromParent();
        continue;
      }

      if (IsIndirectCall(User->getOpcode())) {
        // Replace indirect register operand with more efficient local
        // PC-relative access

        // Note that GV can't be GOT_PREL because it is in the same
        // (anonymous) bin
        LLVMContext *Context = &MF->getFunction()->getContext();
        unsigned ARMPCLabelIndex = AFI->createPICLabelUId();
        unsigned PCAdj = Subtarget->isThumb() ? 4 : 8;
        ARMConstantPoolConstant *CPV = ARMConstantPoolConstant::Create(
            GV, ARMPCLabelIndex, ARMCP::CPValue, PCAdj,
            ARMCP::no_modifier, false);

        unsigned ConstAlign =
          MF->getDataLayout().getPrefTypeAlignment(Type::getInt32PtrTy(*Context));
        unsigned Idx = MF->getConstantPool()->getConstantPoolIndex(CPV, ConstAlign);

        unsigned TempReg = MF->getRegInfo().createVirtualRegister(&ARM::rGPRRegClass);
        unsigned Opc = isThumb2 ? ARM::t2LDRpci : ARM::LDRcp;
        MachineInstrBuilder MIB =
          BuildMI(*User->getParent(), *User, User->getDebugLoc(), TII->get(Opc), TempReg)
          .addConstantPoolIndex(Idx);
        if (Opc == ARM::LDRcp)
          MIB.addImm(0);
        MIB.add(predOps(ARMCC::AL));

        // Fix the address by adding pc.
        // FIXME: this is ugly
        unsigned DestReg = MRI.createVirtualRegister(
            TLI->getRegClassFor(TLI->getPointerTy(MF->getDataLayout())));
        Opc = Subtarget->isThumb() ? ARM::tPICADD : ARM::PICADD;
        MIB = BuildMI(*User->getParent(), *User, User->getDebugLoc(), TII->get(Opc), DestReg)
          .addReg(TempReg)
          .addImm(ARMPCLabelIndex);
        if (!Subtarget->isThumb())
          MIB.add(predOps(ARMCC::AL));

        // Replace register operand
        User->getOperand(0).setReg(DestReg);
      } else {
        unsigned CallOpc = NormalizeCallOpcode(User->getOpcode());
        MachineInstrBuilder MIB = BuildMI(*User->getParent(), *User,
                                          User->getDebugLoc(), TII->get(CallOpc));
        int OpNum = 1;
        if (CallOpc == ARM::tBL) {
          MIB.add(predOps(ARMCC::AL));
          OpNum += 2;
        }
        MIB.addGlobalAddress(GV, 0, 0);
        for (int e = User->getNumOperands(); OpNum < e; ++OpNum)
          MIB.add(User->getOperand(OpNum));
        User->eraseFromParent();
      }
    }
  }
}

void ARMPOTOpt::deleteOldCPEntries(SmallVectorImpl<int> &CPEntries) {
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

bool ARMPOTOpt::isSameBin(const GlobalValue *GV) {
  auto F = dyn_cast<Function>(GV);
  return F && F->isPagerando() && F->getSectionPrefix() == CurBinPrefix;
}
