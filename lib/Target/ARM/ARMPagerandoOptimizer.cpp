//===-- ARMPagerandoOptimizer.cpp - Optimizes intra-bin function calls ----===//
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
// The implementation relies on SSA form to follow def-use chains, therefore,
// this pass must be scheduled before register allocation.
//
//===----------------------------------------------------------------------===//

#include "ARM.h"
#include "ARMBaseInstrInfo.h"
#include "ARMBaseRegisterInfo.h"
#include "ARMConstantPoolValue.h"
#include "ARMMachineFunctionInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Target/TargetLowering.h"

using namespace llvm;

#define DEBUG_TYPE "pagerando"

namespace {
class ARMPagerandoOptimizer : public MachineFunctionPass {
public:
  static char ID;
  explicit ARMPagerandoOptimizer() : MachineFunctionPass(ID) {
    initializeARMPagerandoOptimizerPass(*PassRegistry::getPassRegistry());
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::TracksLiveness);
  }

private:
  void optimizeCalls(MachineInstr *MI, const Function *Callee);
  void replaceWithDirectCall(MachineInstr *MI, const Function *Callee);
  void changeToPCRelativeCall(MachineInstr *MI, const Function *Callee);
  void deleteCPEntries(MachineFunction &MF, const SmallSet<int, 8> &CPIndices);
};
} // end anonymous namespace

char ARMPagerandoOptimizer::ID = 0;
INITIALIZE_PASS(ARMPagerandoOptimizer, "pagerando-optimizer-arm",
                "Pagerando intra-bin optimizer for ARM", false, false)

FunctionPass *llvm::createARMPagerandoOptimizerPass() {
  return new ARMPagerandoOptimizer();
}

static bool isIntraBin(const MachineConstantPoolEntry &E, StringRef BinPrefix) {
  if (!E.isMachineConstantPoolEntry()) return false;

  // ARMConstantPoolValue lacks casting infrastructure to use dyn_cast directly
  auto *CPV = static_cast<ARMConstantPoolValue*>(E.Val.MachineCPVal);
  auto *CPC = dyn_cast<ARMConstantPoolConstant>(CPV);
  if (!CPC) return false;

  auto M = CPC->getModifier();
  auto *F = dyn_cast_or_null<Function>(CPC->getGV());

  return (M == ARMCP::POTOFF || M == ARMCP::BINOFF)
      && F && F->getSectionPrefix() == BinPrefix;
}

static const Function *getCallee(const MachineConstantPoolEntry &E) {
  auto *CPC = static_cast<ARMConstantPoolConstant*>(E.Val.MachineCPVal);
  return cast<Function>(CPC->getGV());
}

static int getCPIndex(const MachineInstr &MI) {
  if (MI.mayLoad() && MI.getNumOperands() > 1 && MI.getOperand(1).isCPI())
    return MI.getOperand(1).getIndex();
  return -1;
}

bool ARMPagerandoOptimizer::runOnMachineFunction(MachineFunction &MF) {
  auto &F = *MF.getFunction();
  // This pass is an optimization (optional), therefore check skipFunction
  if (!F.isPagerando() || skipFunction(F)) {
    return false;
  }

  // Section prefix is assigned by PagerandoBinning pass
  auto BinPrefix = F.getSectionPrefix().getValue();
  auto &CPEntries = MF.getConstantPool()->getConstants();

  // Find intra-bin CP entries
  SmallSet<int, 8> CPIndices;
  int Index = 0;
  for (auto &E : CPEntries) {
    if (isIntraBin(E, BinPrefix))
      CPIndices.insert(Index);
    Index++;
  }

  if (CPIndices.empty())
    return false;

  // Collect uses of intra-bin CP entries
  std::vector<MachineInstr*> Uses;
  for (auto &BB : MF) {
    for (auto &MI : BB) {
      int Index = getCPIndex(MI);
      if (CPIndices.count(Index))
        Uses.push_back(&MI);
    }
  }

  // Optimize intra-bin calls
  for (auto *MI : Uses) {
    int Index = getCPIndex(*MI);
    auto *Callee = getCallee(CPEntries[Index]);
    optimizeCalls(MI, Callee);
  }

  deleteCPEntries(MF, CPIndices);

  return true;
}

static bool isBXCall(unsigned Opc) {
  return Opc == ARM::BX_CALL || Opc == ARM::tBX_CALL;
}

void ARMPagerandoOptimizer::optimizeCalls(MachineInstr *MI,
                                          const Function *Callee) {
  auto &MRI = MI->getParent()->getParent()->getRegInfo();

  SmallVector<MachineInstr*, 4> Queue{MI};
  while (!Queue.empty()) {
    MI = Queue.pop_back_val();

    if (!MI->isCall()) { // Not a call, enqueue users
      for (auto &Op : MI->defs()) {
        for (auto &User : MRI.use_instructions(Op.getReg()))
          Queue.push_back(&User);
      }
      MI->eraseFromParent();
    } else if (isBXCall(MI->getOpcode())) {
      changeToPCRelativeCall(MI, Callee);
    } else { // Standard indirect call
      replaceWithDirectCall(MI, Callee);
    }
  }
}

static unsigned toDirectCall(unsigned Opc) {
  switch (Opc) {
  case ARM::BLX:        return ARM::BL;
  case ARM::tBLXr:      return ARM::tBL;
  case ARM::TCRETURNri: return ARM::TCRETURNdi;
  default:
    llvm_unreachable("Unhandled ARM call opcode");
  }
}

void ARMPagerandoOptimizer::replaceWithDirectCall(MachineInstr *MI,
                                               const Function *Callee) {
  auto &MBB = *MI->getParent();
  auto &TII = *MBB.getParent()->getSubtarget().getInstrInfo();

  auto Opc = toDirectCall(MI->getOpcode());
  auto MIB = BuildMI(MBB, *MI, MI->getDebugLoc(), TII.get(Opc));

  int SkipOps = 1;
  if (MI->getOpcode() == ARM::tBLXr) { // Short instruction
    auto CondOp = predOps(ARMCC::AL);
    MIB.add(CondOp);
    SkipOps += CondOp.size();
  }
  MIB.addGlobalAddress(Callee);

  // Copy over remaining operands
  auto RemainingOps = make_range(MI->operands_begin() + SkipOps,
                                 MI->operands_end());
  for (auto &Op : RemainingOps)
    MIB.add(Op);

  MI->eraseFromParent();
}

// Replace indirect register operand with more efficient PC-relative access
void ARMPagerandoOptimizer::changeToPCRelativeCall(MachineInstr *MI,
                                                   const Function *Callee) {
  auto &MBB = *MI->getParent();
  auto &MF = *MBB.getParent();
  auto &C = MF.getFunction()->getContext();
  auto &AFI = *MF.getInfo<ARMFunctionInfo>();
  auto &TII = *MF.getSubtarget().getInstrInfo();
  auto &TLI = *MF.getSubtarget().getTargetLowering();
  auto &DL = MF.getDataLayout();
  auto &MRI = MF.getRegInfo();
  auto isThumb = AFI.isThumbFunction();

  // Create updated CP entry for callee
  auto Label = AFI.createPICLabelUId();
  auto PCAdj = isThumb ? 4 : 8;
  auto *CPV = ARMConstantPoolConstant::Create(
      Callee, Label, ARMCP::CPValue, PCAdj, ARMCP::no_modifier, false);
  auto Alignment = DL.getPrefTypeAlignment(Type::getInt32PtrTy(C));
  auto Index = MF.getConstantPool()->getConstantPoolIndex(CPV, Alignment);

  // Load callee offset into register
  auto Opc = AFI.isThumb2Function() ? ARM::t2LDRpci : ARM::LDRcp;
  auto OffsetReg = MRI.createVirtualRegister(&ARM::rGPRRegClass);
  auto MIB = BuildMI(MBB, *MI, MI->getDebugLoc(), TII.get(Opc), OffsetReg)
      .addConstantPoolIndex(Index);
  if (Opc == ARM::LDRcp) MIB.addImm(0);
  MIB.add(predOps(ARMCC::AL));

  // Compute callee address by adding PC
  // FIXME: this is ugly // comment by Stephen
  auto RegClass = TLI.getRegClassFor(TLI.getPointerTy(DL));
  auto AddressReg = MRI.createVirtualRegister(RegClass);
  Opc = isThumb ? ARM::tPICADD : ARM::PICADD;
  MIB = BuildMI(MBB, *MI, MI->getDebugLoc(), TII.get(Opc), AddressReg)
      .addReg(OffsetReg)
      .addImm(Label);
  if (!isThumb) MIB.add(predOps(ARMCC::AL));

  // Replace register operand
  MI->getOperand(0).setReg(AddressReg);
}

void ARMPagerandoOptimizer::deleteCPEntries(MachineFunction &MF,
                                         const SmallSet<int, 8> &CPIndices) {
  auto *CP = MF.getConstantPool();
  int Size = CP->getConstants().size();
  int Indices[Size];

  // Create CP index mapping: Indices[Old] -> New
  for (int Old = 0, New = 0; Old < Size; ++Old) {
    Indices[Old] = CPIndices.count(Old) ? -1 : New++;
  }

  // Update remaining (inter-bin) CP references
  for (auto &BB : MF) {
    for (auto &MI : BB) {
      for (auto &Op : MI.explicit_uses()) {
        if (Op.isCPI()) {
          int Old = Op.getIndex();
          int New = Indices[Old];
          assert (New != -1 && "CP entry use should have been deleted");
          Op.setIndex(New);
        }
      }
    }
  }

  // Delete now unreferenced (intra-bin) CP entries (in reverse order so
  // deletion does not affect the index of future deletions)
  for (int Old = Size - 1; Old >= 0; --Old) {
    if (Indices[Old] == -1)
      CP->eraseIndex(Old);
  }
}
