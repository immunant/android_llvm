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
  struct CPEntry {
    const Function *Callee;
    SmallVector<MachineInstr*, 2> Uses;
  };

  MachineFunction *MF;
  const MachineModuleInfo *MMI;
  const TargetInstrInfo *TII;
  const TargetLowering *TLI;
  ARMFunctionInfo *AFI;
  const ARMSubtarget *Subtarget;
  StringRef CurBinPrefix;
  MachineConstantPool *ConstantPool;
  bool isThumb2;

  void replaceUse(MachineInstr *MI, const Function *Callee);
  void deleteEntries(const std::map<int, CPEntry> &Worklist);
};
} // end anonymous namespace

char PagerandoOptimizer::ID = 0;
INITIALIZE_PASS(PagerandoOptimizer, "pagerando-optimizer",
                "Pagerando intra-bin optimizer", false, false)

FunctionPass *llvm::createPagerandoOptimizerPass() {
  return new PagerandoOptimizer();
}

static std::tuple<bool, const Function*>
isIntraBin(const MachineConstantPoolEntry &E, StringRef BinPrefix) {
  if (!E.isMachineConstantPoolEntry()) return {false, nullptr};

  // ARMConstantPoolValue lacks casting infrastructure to use dyn_cast directly
  auto *CPV = static_cast<ARMConstantPoolValue*>(E.Val.MachineCPVal);
  auto *CPC = dyn_cast<ARMConstantPoolConstant>(CPV);
  if (!CPC) return {false, nullptr};

  auto M = CPC->getModifier();
  auto *F = dyn_cast_or_null<Function>(CPC->getGV());
  bool intraBin = (M == ARMCP::POTOFF || M == ARMCP::BINOFF)
                  && F && F->getSectionPrefix() == BinPrefix;
  return {intraBin, F};
}

static int getConstantPoolIndex(const MachineInstr &MI) {
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

  // TODO(yln): maybe used IndexedMap, make sure it is sorted (so we can safely delete in reverse order)
  std::map<int, CPEntry> Worklist;

  // Find intra-bin constant pool entries
  for (int Index = 0; Index < CPEntries.size(); ++Index) {
    bool intraBin; const Function *F;
    std::tie(intraBin, F) = isIntraBin(CPEntries[Index], CurBinPrefix);
    if (intraBin) {
      Worklist.emplace(Index, CPEntry{F, {}});
    }
  }

  if (Worklist.empty()) {
    return false;
  }

  // Collect uses of intra-bin constant pool entries
  for (auto &BB : *MF) {
    for (auto &MI : BB) {
      auto I = Worklist.find(getConstantPoolIndex(MI));
      if (I != Worklist.end()) {
        I->second.Uses.push_back(&MI);
      }
    }
  }

  for (auto &E : Worklist) {
    for (auto *MI : E.second.Uses) {
      replaceUse(MI, E.second.Callee);
    }
  }

  deleteEntries(Worklist);

  return true;
}

static bool isBXCall(unsigned Opc) {
  return Opc == ARM::BX_CALL || Opc == ARM::tBX_CALL;
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

void PagerandoOptimizer::replaceUse(MachineInstr *MI, const Function *Callee) {
  MachineRegisterInfo &MRI = MF->getRegInfo();

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

    if (isBXCall(User->getOpcode())) {
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
    } else { // indirect call -> direct call
      // Replace users of POT-indirect CP entries with direct calls
      unsigned CallOpc = toDirectCall(User->getOpcode());
      MachineInstrBuilder MIB = BuildMI(*User->getParent(), *User,
                                        User->getDebugLoc(), TII->get(CallOpc));
      int OpNum = 1;
      if (CallOpc == ARM::tBL) {
        MIB.add(predOps(ARMCC::AL));
        OpNum += 2;
      }
      MIB.addGlobalAddress(Callee, 0, 0);
      for (int e = User->getNumOperands(); OpNum < e; ++OpNum)
        MIB.add(User->getOperand(OpNum));
      User->eraseFromParent();
    }
  }
}

void PagerandoOptimizer::deleteEntries(const std::map<int, CPEntry> &Worklist) {
  size_t Size = ConstantPool->getConstants().size();
  int Indices[Size];

  // Create CP index mapping: Indices[Old] -> New
  for (int Old = 0, New = 0; Old < Size; ++Old) {
    auto I = Worklist.find(Old);
    Indices[Old] = (I != Worklist.end()) ? -1 : New++;
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
  // deletions do not affect the index of future deletions)
  for (auto I = Worklist.rbegin(), E = Worklist.rend(); I != E; ++I) {
    auto Old = static_cast<unsigned>(I->first);
    ConstantPool->eraseIndex(Old);
  }
}
