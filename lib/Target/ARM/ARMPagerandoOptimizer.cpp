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

void PagerandoOptimizer::optimizeCall(MachineInstr *MI, const Function *Callee) {
  auto *MF = MI->getParent()->getParent();
  auto &MRI = MF->getRegInfo();

  SmallVector<MachineInstr*, 4> Queue{MI};

  while (!Queue.empty()) {
    MI = Queue.pop_back_val();

    if (!MI->isCall()) {
      for (auto &Op : MI->defs()) {
        for (auto &User : MRI.use_instructions(Op.getReg())) {
          Queue.push_back(&User);
        }
      }
      MI->eraseFromParent();
      continue;
    } else  if (isBXCall(MI->getOpcode())) {
      // Replace indirect register operand with more efficient local
      // PC-relative access

      // Note that GV can't be GOT_PREL because it is in the same
      // (anonymous) bin
      LLVMContext *Context = &MF->getFunction()->getContext();
      unsigned ARMPCLabelIndex = AFI->createPICLabelUId();
      unsigned PCAdj = Subtarget->isThumb() ? 4 : 8;
      ARMConstantPoolConstant *CPV = ARMConstantPoolConstant::Create(
          Callee, ARMPCLabelIndex, ARMCP::CPValue, PCAdj,
          ARMCP::no_modifier, false);

      unsigned ConstAlign =
          MF->getDataLayout().getPrefTypeAlignment(Type::getInt32PtrTy(*Context));
      unsigned Idx = MF->getConstantPool()->getConstantPoolIndex(CPV, ConstAlign);

      unsigned TempReg = MF->getRegInfo().createVirtualRegister(&ARM::rGPRRegClass);
      unsigned Opc = isThumb2 ? ARM::t2LDRpci : ARM::LDRcp;
      MachineInstrBuilder MIB =
          BuildMI(*MI->getParent(), *MI, MI->getDebugLoc(), TII->get(Opc), TempReg)
              .addConstantPoolIndex(Idx);
      if (Opc == ARM::LDRcp)
        MIB.addImm(0);
      MIB.add(predOps(ARMCC::AL));

      // Fix the address by adding pc.
      // FIXME: this is ugly
      unsigned DestReg = MRI.createVirtualRegister(
          TLI->getRegClassFor(TLI->getPointerTy(MF->getDataLayout())));
      Opc = Subtarget->isThumb() ? ARM::tPICADD : ARM::PICADD;
      MIB = BuildMI(*MI->getParent(), *MI, MI->getDebugLoc(), TII->get(Opc), DestReg)
          .addReg(TempReg)
          .addImm(ARMPCLabelIndex);
      if (!Subtarget->isThumb())
        MIB.add(predOps(ARMCC::AL));

      // Replace register operand
      MI->getOperand(0).setReg(DestReg);
    } else {
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
    llvm_unreachable("Unhandled ARM call opcode.");
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
  for (int e = MI->getNumOperands(); OpNum < e; ++OpNum) {
    MIB.add(MI->getOperand(OpNum));
  }

  MI->eraseFromParent();
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
