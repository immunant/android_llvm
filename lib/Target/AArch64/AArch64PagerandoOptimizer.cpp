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
//#include "ARMBaseInstrInfo.h"
//#include "ARMBaseRegisterInfo.h"
//#include "ARMConstantPoolValue.h"
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
  explicit AArch64PagerandoOptimizer() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::TracksLiveness);
  }

private:
  void optimizeCall(MachineInstr *MI, const Function *Callee);
  void replaceWithDirectCall(MachineInstr *MI, const Function *Callee);
//  void replaceWithPCRelativeCall(MachineInstr *MI, const Function *Callee);
//  void deleteCPEntries(MachineFunction &MF, const SmallSet<int, 8> &Workset);
};
} // end anonymous namespace

char AArch64PagerandoOptimizer::ID = 0;
INITIALIZE_PASS(AArch64PagerandoOptimizer, "pagerando-optimizer-aarch64",
                "Pagerando intra-bin optimizer for AArch64", false, false)

FunctionPass *llvm::createAArch64PagerandoOptimizerPass() {
  return new AArch64PagerandoOptimizer();
}

//static bool isIntraBin(const MachineConstantPoolEntry &E, StringRef BinPrefix) {
//  if (!E.isMachineConstantPoolEntry()) return false;
//
//  // ARMConstantPoolValue lacks casting infrastructure to use dyn_cast directly
//  auto *CPV = static_cast<ARMConstantPoolValue*>(E.Val.MachineCPVal);
//  auto *CPC = dyn_cast<ARMConstantPoolConstant>(CPV);
//  if (!CPC) return false;
//
//  auto M = CPC->getModifier();
//  auto *F = dyn_cast_or_null<Function>(CPC->getGV());
//
//  return (M == ARMCP::POTOFF || M == ARMCP::BINOFF)
//      && F && F->getSectionPrefix() == BinPrefix;
//}
//
//static const Function *getCallee(const MachineConstantPoolEntry &E) {
//  auto *CPC = static_cast<ARMConstantPoolConstant*>(E.Val.MachineCPVal);
//  return cast<Function>(CPC->getGV());
//}
//
//static int getCPIndex(const MachineInstr &MI) {
//  if (MI.mayLoad() && MI.getNumOperands() > 1 && MI.getOperand(1).isCPI()) {
//    return MI.getOperand(1).getIndex();
//  }
//  return -1;
//}

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
    errs() << "before optimze:\n";
    MI->getParent()->dump();
    optimizeCall(MI, Callee);
    errs() << "AFTER optimze:\n";
    MI->getParent()->dump();
  }
//
//  deleteCPEntries(MF, Workset);
//
  return true;
}
//
//static bool isBXCall(unsigned Opc) {
//  return Opc == ARM::BX_CALL || Opc == ARM::tBX_CALL;
//}

void AArch64PagerandoOptimizer::optimizeCall(MachineInstr *MI,
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
      MI->eraseFromParent();
//    } else if (isBXCall(MI->getOpcode())) {
//      replaceWithPCRelativeCall(MI, Callee);
    } else { // Standard indirect call
      replaceWithDirectCall(MI, Callee);
    }
  }
}

static unsigned toDirectCall(unsigned Opc) {
  switch (Opc) {
  case AArch64::BLR:  return AArch64::BL;
//  case ARM::TCRETURNri: return ARM::TCRETURNdi;
//  case ARM::BLX:        return ARM::BL;
//  case ARM::tBLXr:      return ARM::tBL;
  default:
    llvm_unreachable("Unhandled ARM call opcode");
  }
}

void AArch64PagerandoOptimizer::replaceWithDirectCall(MachineInstr *MI,
                                               const Function *Callee) {
  auto &MBB = *MI->getParent();
  auto &TII = *MBB.getParent()->getSubtarget().getInstrInfo();

  auto Opc = toDirectCall(MI->getOpcode());
  auto MIB = BuildMI(MBB, *MI, MI->getDebugLoc(), TII.get(Opc))
      .addGlobalAddress(Callee);

  int SkipOps = 1;
//  if (MI->getOpcode() == ARM::tBLXr) { // Short instruction
//    auto CondOp = predOps(ARMCC::AL);
//    MIB.add(CondOp);
//    SkipOps += CondOp.size();
//  }
//  MIB.addGlobalAddress(Callee);
//
  // Copy over remaining operands
  auto RemainingOps = make_range(MI->operands_begin() + SkipOps,
                                 MI->operands_end());
  for (auto &Op : RemainingOps) {
    MIB.add(Op);
  }

  MI->eraseFromParent();
}

//// Replace indirect register operand with more efficient PC-relative access
//void AArch64PagerandoOptimizer::replaceWithPCRelativeCall(MachineInstr *MI,
//                                                   const Function *Callee) {
//  auto &MBB = *MI->getParent();
//  auto &MF = *MBB.getParent();
//  auto &C = MF.getFunction()->getContext();
//  auto &AFI = *MF.getInfo<ARMFunctionInfo>();
//  auto &TII = *MF.getSubtarget().getInstrInfo();
//  auto &TLI = *MF.getSubtarget().getTargetLowering();
//  auto &DL = MF.getDataLayout();
//  auto &MRI = MF.getRegInfo();
//  auto isThumb = AFI.isThumbFunction();
//
//  // Create updated CP entry for callee
//  auto Label = AFI.createPICLabelUId();
//  auto PCAdj = isThumb ? 4 : 8;
//  auto *CPV = ARMConstantPoolConstant::Create(
//      Callee, Label, ARMCP::CPValue, PCAdj, ARMCP::no_modifier, false);
//  auto Alignment = DL.getPrefTypeAlignment(Type::getInt32PtrTy(C));
//  auto Index = MF.getConstantPool()->getConstantPoolIndex(CPV, Alignment);
//
//  // Load callee offset into register
//  auto Opc = AFI.isThumb2Function() ? ARM::t2LDRpci : ARM::LDRcp;
//  auto OffsetReg = MRI.createVirtualRegister(&ARM::rGPRRegClass);
//  auto MIB = BuildMI(MBB, *MI, MI->getDebugLoc(), TII.get(Opc), OffsetReg)
//      .addConstantPoolIndex(Index);
//  if (Opc == ARM::LDRcp) MIB.addImm(0);
//  MIB.add(predOps(ARMCC::AL));
//
//  // Compute callee address by adding PC
//  // FIXME: this is ugly // comment by Stephen
//  auto RegClass = TLI.getRegClassFor(TLI.getPointerTy(DL));
//  auto AddressReg = MRI.createVirtualRegister(RegClass);
//  Opc = isThumb ? ARM::tPICADD : ARM::PICADD;
//  MIB = BuildMI(MBB, *MI, MI->getDebugLoc(), TII.get(Opc), AddressReg)
//      .addReg(OffsetReg)
//      .addImm(Label);
//  if (!isThumb) MIB.add(predOps(ARMCC::AL));
//
//  // Replace register operand
//  MI->getOperand(0).setReg(AddressReg);
//}
//
//void AArch64PagerandoOptimizer::deleteCPEntries(MachineFunction &MF,
//                                         const SmallSet<int, 8> &Workset) {
//  auto *CP = MF.getConstantPool();
//  int Size = CP->getConstants().size();
//  int Indices[Size];
//
//  // Create CP index mapping: Indices[Old] -> New
//  for (int Old = 0, New = 0; Old < Size; ++Old) {
//    Indices[Old] = Workset.count(Old) ? -1 : New++;
//  }
//
//  // Update remaining (inter-bin) CP references
//  for (auto &BB : MF) {
//    for (auto &MI : BB) {
//      for (auto &Op : MI.explicit_uses()) {
//        if (Op.isCPI()) {
//          int Old = Op.getIndex();
//          int New = Indices[Old];
//          assert (New != -1 && "CP entry use should have been deleted");
//          Op.setIndex(New);
//        }
//      }
//    }
//  }
//
//  // Delete now unreferenced (intra-bin) CP entries (in reverse order so
//  // deletion does not affect the index of future deletions)
//  for (int Old = Size - 1; Old >= 0; --Old) {
//    if (Indices[Old] == -1) {
//      CP->eraseIndex(Old);
//    }
//  }
//}
