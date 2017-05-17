//===-- PGLTEntryWrappers.cpp: PGLT base address entry wrapper pass -------===//
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
// Copyright 2016, 2017 Immunant, Inc.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace llvm;

#define DEBUG_TYPE "pglt"
static const char *const kOrigFnSuffix = "$$orig";
static const char *const kOrigVAFnSuffix = "$$origva";

namespace {
class PGLTEntryWrappers : public ModulePass {
public:
  static char ID; // Pass identification, replacement for typeid
  explicit PGLTEntryWrappers() : ModulePass(ID) {
    initializePGLTEntryWrappersPass(*PassRegistry::getPassRegistry());
  }
  bool runOnModule(Module &M) override;

  StringRef getPassName() const override { return "PGLT Base Address entry point wrapper pass"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
  }

private:
  bool ProcessFn(Function &F);
  Function* CreateWrapper(Function &F);
  Function* RewriteVarargs(Function &F, IRBuilder<> &Builder, Value *&VAList);
  void MoveInstructionToWrapper(Instruction *I, BasicBlock *BB);
  void CreatePGLT(Module &M);
};
} // end anonymous namespace

bool PGLTEntryWrappers::runOnModule(Module &M) {
  bool Changed = false;

  std::vector<Function*> Functions;
  for (auto &F : M)
    Functions.push_back(&F);

  for (auto F : Functions)
    Changed |= ProcessFn(*F);

  if (Changed)
    CreatePGLT(M);

  // DEBUG(M.dump());

  return Changed;
}

bool PGLTEntryWrappers::ProcessFn(Function &F) {
  if (F.isDeclaration() || F.hasAvailableExternallyLinkage() ||
      F.hasComdat()) // TODO: Support COMDAT
    return false;

  if (isa<UnreachableInst>(F.getEntryBlock().getTerminator()))
    return false;

  F.addFnAttr(Attribute::RandPage);

  std::vector<Use *> AddressUses;
  SmallSet<User*, 5> Users;
  for (Use &U : F.uses()) {
    User *FU = U.getUser();
    if (isa<BlockAddress>(FU)) {
      // This is handled in AsmPrinter::EmitBasicBlockStart
      continue;
    }
    if (isa<GlobalAlias>(FU)) {
      // These do not need to be indirected
      continue;
    }
    if (isa<Constant>(FU)) {
      if (Users.count(FU) == 1)
        continue; // we will replace all uses in this user at once

      // Don't replace calls to bitcasts of function symbols, since they get
      // translated to direct calls.
      if (ConstantExpr *CE = dyn_cast<ConstantExpr>(FU)) {
        if (CE->getOpcode() == Instruction::BitCast) {
          // This bitcast must have exactly one user.
          if (CE->user_begin() != CE->user_end()) {
            User *ParentUs = *CE->user_begin();
            if (CallInst *CI = dyn_cast<CallInst>(ParentUs)) {
              CallSite CS(CI);
              Use &CEU = *CE->use_begin();
              if (CS.isCallee(&CEU)) {
                continue;
              }
            }
            if (isa<GlobalAlias>(ParentUs))
              continue;
          }
        }
      }
    }

    if (isa<CallInst>(FU) || isa<InvokeInst>(FU)) {
      ImmutableCallSite CS(cast<Instruction>(FU));
      if (CS.isCallee(&U))
        continue;
    }

    AddressUses.push_back(&U);
    Users.insert(FU);
  }

  std::sort(AddressUses.begin(), AddressUses.end());
  std::unique(AddressUses.begin(), AddressUses.end());

  if (!AddressUses.empty() || !F.hasLocalLinkage()) {
    Function* WrapperFn = CreateWrapper(F);

    if (WrapperFn->hasLocalLinkage() && !WrapperFn->isVarArg()) {
      // Any address-taken uses may escape the module, so we need to replace them
      // with the wrapper.
      for (auto U : AddressUses) {
        // Have we replaced this use?
        if (!U->get()) continue;

        if (auto GA = dyn_cast<GlobalAlias>(U->getUser())) {
          GA->setAliasee(WrapperFn);
        } else if (auto GV = dyn_cast<GlobalVariable>(U->getUser())) {
          assert(GV->getInitializer() == &F);
          GV->setInitializer(WrapperFn);
        } else if (Constant *C = dyn_cast<Constant>(U->getUser())) {
          C->handleOperandChange(&F, WrapperFn);
        } else {
          U->set(WrapperFn);
        }
      }
    }
    return true;
  }

  return false;
}

Function* PGLTEntryWrappers::CreateWrapper(Function &F) {
  Module *M = F.getParent();
  FunctionType *FFTy = F.getFunctionType();

  Function *WrapperFn = Function::Create(FFTy, F.getLinkage(), F.getName() + "_$wrap", M);
  BasicBlock *BB = BasicBlock::Create(F.getContext(), "", WrapperFn);

  WrapperFn->setCallingConv(F.getCallingConv());
  WrapperFn->copyAttributesFrom(&F);

  WrapperFn->addFnAttr(Attribute::NoInline);
  WrapperFn->addFnAttr(Attribute::OptimizeForSize);

  // TODO: SJC can we place wrappers on randomly located pages? I don't see why
  // not, but this is safer for now
  WrapperFn->removeFnAttr(Attribute::RandPage);
  WrapperFn->addFnAttr(Attribute::RandWrapper);

  // Ensure that the wrapper is not placed in an explicitly named section. If it
  // is, the section flags will be combined with other function in the section
  // (RandPage functions, potentially), and the wrapper will get marked
  // RAND_ADDR

  // TODO: Verify the above. This should not be the case unless functions are not
  // WrapperFn->setSection("");

  if (F.hasComdat()) {
    WrapperFn->setComdat(F.getComdat());
  }

  if (F.hasSection()) {
    WrapperFn->setSection(F.getSection());
    F.setSection("");
  }

  // We can't put the wrapper function in an explicitely named section becuase
  // it then does not get a per-function section, which we need to properly
  // support --gc-sections
  // WrapperFn->setSection(".text.wrappers");

  if (!F.hasLocalLinkage() || F.isVarArg()) {
    std::string OldName = F.getName();
    WrapperFn->takeName(&F);
    F.replaceAllUsesWith(WrapperFn);
    if (F.isVarArg())
      F.setName(OldName + kOrigVAFnSuffix);
    else
      F.setName(OldName + kOrigFnSuffix);

    if (!F.hasLocalLinkage())
      F.setVisibility(GlobalValue::HiddenVisibility);
  }

  IRBuilder<> Builder(BB);

  Value *VAList = nullptr;
  Function *DestFn = &F;
  if (F.isVarArg()) {
    DEBUG(F.dump());
    DestFn = RewriteVarargs(F, Builder, VAList);
  }

  // F may have been deleted at this point. DO NOT USE F!

  // Set the PGLT base address
  auto PGLTAddress = M->getGlobalVariable("_PGLT_");
  if (!PGLTAddress) {
    PGLTAddress = new GlobalVariable(*M, Builder.getInt8Ty(), true,
                                     GlobalValue::ExternalLinkage,
                                     nullptr, "_PGLT_");
    PGLTAddress->setVisibility(GlobalValue::ProtectedVisibility);
  }

  SmallVector<Value *, 16> Args;
  Args.reserve(FFTy->getNumParams());

  unsigned i = 0;
  for (Function::arg_iterator AI = WrapperFn->arg_begin(), AE = WrapperFn->arg_end();
       AI != AE; ++AI) {
    Args.push_back(AI);
    ++i;
  }

  if (VAList)
    Args.push_back(VAList);

  CallInst *CI = Builder.CreateCall(DestFn, Args);
  CI->setCallingConv(WrapperFn->getCallingConv());

  if (WrapperFn->getReturnType()->isVoidTy()) {
    Builder.CreateRetVoid();
  } else {
    Builder.CreateRet(CI);
  }

  DEBUG(WrapperFn->dump());
  DEBUG(DestFn->dump());

  return WrapperFn;
}

static Instruction* findAlloca(Instruction* Use) {
  Instruction *Alloca = Use;
  while (Alloca && !isa<AllocaInst>(Alloca)) {
    Alloca = dyn_cast<Instruction>(Alloca->op_begin());
  }
  assert(Alloca && "Could not find va_list allloc in a var args functions");
  return Alloca;
}

Function* PGLTEntryWrappers::RewriteVarargs(Function &F, IRBuilder<> &Builder,
                                            Value *&VAList) {
  Module *M = F.getParent();
  FunctionType *FFTy = F.getFunctionType();
  Function *NewFn = &F;

  SmallVector<CallInst*, 1> VAStarts;
  for (auto &B : F) {
    for (auto &I : B) {
      if (isa<VAStartInst>(&I)) {
        VAStarts.push_back(cast<CallInst>(&I));
      }
    }
  }

  if (VAStarts.empty())
    return NewFn;

  // Find A va_list alloca. This is really only to get the type.
  // TODO: use a static type
  Instruction *VAListAlloca = findAlloca(VAStarts[0]);

  // Need to create a new function that takes a va_list parameter but is not
  // varargs and clone the original function into it.
  auto VAListTy = VAListAlloca->getType()->getPointerElementType();

  // in the wrapper
  auto *NewVAListAlloca = Builder.CreateAlloca(VAListTy);
  Builder.CreateCall(
      Intrinsic::getDeclaration(M, Intrinsic::vastart),
      {Builder.CreateBitCast(NewVAListAlloca, Builder.getInt8PtrTy())});

  // Create a new function definition
  SmallVector<Type*, 4> Params(FFTy->param_begin(), FFTy->param_end());
  Params.push_back(VAListTy->getPointerTo());
  FunctionType *NonVarArgs = FunctionType::get(FFTy->getReturnType(), Params, false);
  NewFn = Function::Create(NonVarArgs, F.getLinkage(), "", M);
  NewFn->copyAttributesFrom(&F);
  NewFn->setComdat(F.getComdat());
  NewFn->takeName(&F);
  NewFn->setSubprogram(F.getSubprogram());

  // Move the original function blocks into the newly created function
  NewFn->getBasicBlockList().splice(NewFn->begin(), F.getBasicBlockList());

  // Transfer old arguments to new arguments
  Function::arg_iterator NewArgI = NewFn->arg_begin();
  for (Function::arg_iterator OldArgI = F.arg_begin(), OldArgE = F.arg_end();
       OldArgI != OldArgE; ++OldArgI, ++NewArgI) {
    OldArgI->replaceAllUsesWith(&*NewArgI);
    NewArgI->takeName(&*OldArgI);
  }

  ValueToValueMapTy VMap;
  SmallVector<ReturnInst*, 1> Returns;

  Function::arg_iterator DestI = NewFn->arg_begin();
  for (Function::const_arg_iterator I = F.arg_begin(), E = F.arg_end();
       I != E; ++I)
    if (VMap.count(I) == 0) {   // Is this argument preserved?
      DestI->setName(I->getName()); // Copy the name over...
      VMap[I] = DestI++;        // Add mapping to VMap
    }

  // return the new wrapper alloca to our caller so it can pass it in the built
  // call
  VAList = NewVAListAlloca;

  // Optimized for a single va_start() call. We can remove the va_list alloca
  // and va_start, and simply use the parameter directly. If there is more than
  // one va_start, then we need to keep the alloca and replace va_start with a
  // va_copy.
  if (VAStarts.size() == 1) {
    // use the new va_list argument instead of an alloca in the callee
    VAListAlloca->replaceAllUsesWith(NewArgI);
    VAListAlloca->eraseFromParent();

    // remove the va_start
    VAStarts[0]->eraseFromParent();
  } else {
    IRBuilder<> CalleeBuilder(NewFn->getContext());
    for (auto NewI : VAStarts) {
      CalleeBuilder.SetInsertPoint(NewI);
      CalleeBuilder.CreateCall(
          Intrinsic::getDeclaration(M, Intrinsic::vacopy),
          {NewI->getArgOperand(0),
           CalleeBuilder.CreateBitCast(NewArgI, CalleeBuilder.getInt8PtrTy())});

      // remove the va_start
      NewI->eraseFromParent();
    }
  }

  F.eraseFromParent();

  return NewFn;
}

void PGLTEntryWrappers::MoveInstructionToWrapper(Instruction *I, BasicBlock *BB) {
  for (auto &U : I->operands()) {
    if (auto UI = dyn_cast<Instruction>(U.get())) {
      MoveInstructionToWrapper(UI, BB);
    }
  }

  if (isa<BitCastInst>(I))
    I = I->clone();
  else
    I->removeFromParent();

  BB->getInstList().push_back(I);
}

void PGLTEntryWrappers::CreatePGLT(Module &M) {
  LLVMContext &C = M.getContext();
  auto *PtrTy = Type::getInt8PtrTy(C);
  auto *PGLTType = ArrayType::get(PtrTy, 1);

  Constant *Initializer = ConstantArray::get(PGLTType, {ConstantPointerNull::get(PtrTy)});
  auto *PGLT = new GlobalVariable(M, PGLTType, true, GlobalValue::ExternalLinkage,
                                  Initializer, "llvm.pglt");
  PGLT->setVisibility(GlobalValue::ProtectedVisibility);

  GlobalVariable *LLVMUsed = M.getGlobalVariable("llvm.used");
  std::vector<Constant *> MergedVars;
  if (LLVMUsed) {
    // Collect the existing members of llvm.used.
    ConstantArray *Inits = cast<ConstantArray>(LLVMUsed->getInitializer());
    for (unsigned I = 0, E = Inits->getNumOperands(); I != E; ++I)
      MergedVars.push_back(Inits->getOperand(I));
    LLVMUsed->eraseFromParent();
  }

  Type *i8PTy = Type::getInt8PtrTy(C);
  // Add uses for pglt
  MergedVars.push_back(
      ConstantExpr::getBitCast(PGLT, i8PTy));

  // Recreate llvm.used.
  ArrayType *ATy = ArrayType::get(i8PTy, MergedVars.size());
  LLVMUsed =
      new GlobalVariable(M, ATy, false, GlobalValue::AppendingLinkage,
                         ConstantArray::get(ATy, MergedVars), "llvm.used");

  LLVMUsed->setSection("llvm.metadata");
}

char PGLTEntryWrappers::ID = 0;
INITIALIZE_PASS(PGLTEntryWrappers, "pglt-entry-wrappers",
                "PGLT Entry Wrappers", false, false)

ModulePass *llvm::createPGLTEntryWrappersPass() {
  return new PGLTEntryWrappers();
}
