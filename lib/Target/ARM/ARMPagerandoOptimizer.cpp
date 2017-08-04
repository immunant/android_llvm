//===----------------------------------------------------------------------===//
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

namespace {
class PagerandoOptimizer : public MachineFunctionPass {
public:
  static char ID;
  explicit PagerandoOptimizer() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &Fn) override;

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::TracksLiveness);
  }

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

  void optimizeCall(MachineInstr *MI, const Function *Callee);
  void replaceWithDirectCall(MachineInstr *MI, const Function *Callee);
  void replaceWithPCRelativeCall(MachineInstr *MI, const Function *Callee);
  void deleteCPEntries(const SmallSet<int, 8> &Workset);
};
} // end anonymous namespace

char PagerandoOptimizer::ID = 0;
INITIALIZE_PASS(PagerandoOptimizer, "pagerando-optimizer",
                "Pagerando intra-bin optimizer", false, false)

FunctionPass *llvm::createPagerandoOptimizerPass() {
  return new PagerandoOptimizer();
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
      && F &&F->getSectionPrefix() == BinPrefix;
}

static const Function *getCallee(const MachineConstantPoolEntry &E) {
  auto *CPC = static_cast<ARMConstantPoolConstant*>(E.Val.MachineCPVal);
  return cast<Function>(CPC->getGV());
}

static int getCPIndex(const MachineInstr &MI) {
  if (MI.mayLoad() && MI.getNumOperands() > 1 && MI.getOperand(1).isCPI()) {
    return MI.getOperand(1).getIndex();
  }
  return -1;
}

bool PagerandoOptimizer::runOnMachineFunction(MachineFunction &Fn) {
  // This pass is an optimization (optional), therefore check skipFunction.
  if (skipFunction(*Fn.getFunction()) || !Fn.getFunction()->isPagerando()) {
    return false;
  }

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

  auto &CPEntries = ConstantPool->getConstants();

  SmallSet<int, 8> Workset;

  // Find intra-bin CP entries
  for (int Index = 0; Index < CPEntries.size(); ++Index) {
    bool intraBin = isIntraBin(CPEntries[Index], CurBinPrefix);
    if (intraBin) {
      Workset.insert(Index);
    }
  }

  if (Workset.empty()) {
    return false;
  }

  std::vector<MachineInstr*> Uses;

  // Collect uses of intra-bin CP entries
  for (auto &BB : *MF) {
    for (auto &MI : BB) {
      int Index = getCPIndex(MI);
      if (Workset.count(Index)) {
        Uses.push_back(&MI);
      }
    }
  }

  // Optimize intra-bin calls
  for (auto *MI : Uses) {
    int Index = getCPIndex(*MI);
    auto Callee = getCallee(CPEntries[Index]);
    optimizeCall(MI, Callee);
  }

  deleteCPEntries(Workset);

  return true;
}

static bool isBXCall(unsigned Opc) {
  return Opc == ARM::BX_CALL || Opc == ARM::tBX_CALL;
}

void PagerandoOptimizer::optimizeCall(MachineInstr *MI,
                                      const Function *Callee) {
  auto &MRI = MF->getRegInfo();

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
    } else if (isBXCall(MI->getOpcode())) {
      replaceWithPCRelativeCall(MI, Callee);
    } else { // Standard indirect call
      replaceWithDirectCall(MI, Callee);
    }
  }
}

static unsigned toDirectCall(unsigned Opc) {
  switch (Opc) {
  case ARM::TCRETURNri: return ARM::TCRETURNdi;
  case ARM::BLX:        return ARM::BL;
  case ARM::tBLXr:      return ARM::tBL;
  default:
    llvm_unreachable("Unhandled ARM call opcode");
  }
}

void PagerandoOptimizer::replaceWithDirectCall(MachineInstr *MI,
                                               const Function *Callee) {
  auto Opc = toDirectCall(MI->getOpcode());
  auto MIB = BuildMI(*MI->getParent(), *MI, MI->getDebugLoc(), TII->get(Opc));

  // TODO(yln): maybe this can be cleaned up by not emitting a conditional branch/link
  unsigned OpNum = 1;
  if (Opc == ARM::tBL) {
    MIB.add(predOps(ARMCC::AL));
    OpNum += 2;
  }
  MIB.addGlobalAddress(Callee);

  // Copy over remaining operands
  for (; OpNum < MI->getNumOperands(); ++OpNum) {
    MIB.add(MI->getOperand(OpNum));
  }

  MI->eraseFromParent();
}

// Replace indirect register operand with more efficient PC-relative access
void PagerandoOptimizer::replaceWithPCRelativeCall(MachineInstr *MI,
                                                   const Function *Callee) {
  auto &MBB = *MI->getParent();
  auto &MF = *MBB.getParent();
  auto &C = MF.getFunction()->getContext();
  auto &DL = MF.getDataLayout();
  auto &MRI = MF.getRegInfo();
  auto isThumb = Subtarget->isThumb();

  // Create updated CP entry for callee
  auto Label = AFI->createPICLabelUId();
  auto PCAdj = isThumb ? 4 : 8;
  auto *CPV = ARMConstantPoolConstant::Create(
      Callee, Label, ARMCP::CPValue, PCAdj, ARMCP::no_modifier, false);
  auto Alignment = DL.getPrefTypeAlignment(Type::getInt32PtrTy(C));
  auto Index = MF.getConstantPool()->getConstantPoolIndex(CPV, Alignment);

  // Load callee offset into register
  bool isThumb2 = MF.getInfo<ARMFunctionInfo>()->isThumb2Function(); // TODO(yln): why do we query Subtarget and ARMFunctionInfo for thumb thing, is there a difference? Both have isThumb, isThumb2...
  auto Opc = isThumb2 ? ARM::t2LDRpci : ARM::LDRcp;
  auto OffsetReg = MRI.createVirtualRegister(&ARM::rGPRRegClass);
  auto MIB = BuildMI(MBB, *MI, MI->getDebugLoc(), TII->get(Opc), OffsetReg)
      .addConstantPoolIndex(Index);
  if (Opc == ARM::LDRcp) MIB.addImm(0);
  MIB.add(predOps(ARMCC::AL));

  // Compute callee address by adding PC
  // FIXME: this is ugly // comment by Stephen
  auto RegClass = TLI->getRegClassFor(TLI->getPointerTy(DL));
  auto AddressReg = MRI.createVirtualRegister(RegClass);
  Opc = isThumb ? ARM::tPICADD : ARM::PICADD;
  MIB = BuildMI(MBB, *MI, MI->getDebugLoc(), TII->get(Opc), AddressReg)
      .addReg(OffsetReg)
      .addImm(Label);
  if (!isThumb) MIB.add(predOps(ARMCC::AL));

  // Replace register operand
  MI->getOperand(0).setReg(AddressReg);
}

void PagerandoOptimizer::deleteCPEntries(const SmallSet<int, 8> &Workset) {
  int Size = ConstantPool->getConstants().size();
  int Indices[Size];

  // Create CP index mapping: Indices[Old] -> New
  for (int Old = 0, New = 0; Old < Size; ++Old) {
    Indices[Old] = Workset.count(Old) ? -1 : New++;
  }

  // Update remaining (inter-bin) CP references
  for (auto &BB : *MF) {
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
    if (Indices[Old] == -1) {
      ConstantPool->eraseIndex(Old);
    }
  }
}
