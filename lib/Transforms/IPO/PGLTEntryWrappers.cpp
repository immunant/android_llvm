//===-- PGLTEntryWrappers.cpp - PGLT base address entry wrapper pass ------===//
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
// Copyright 2016, 2017 Immunant, Inc.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace llvm;

#define DEBUG_TYPE "pglt"

namespace {
class PGLTEntryWrappers : public ModulePass {
public:
  static char ID;
  explicit PGLTEntryWrappers() : ModulePass(ID) {
    initializePGLTEntryWrappersPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    // Requires nothing, preserves nothing
    ModulePass::getAnalysisUsage(AU);
  }

private:
  static constexpr const char *OrigSuffix = "$$orig";
  static constexpr const char *OrigVASuffix = "$$origva";
  static constexpr const char *WrapperSuffix = "$$wrap";

  void ProcessFunction(Function &F);
  void CreateWrapper(Function &F, const std::vector<Use*> &AddressUses);
  void ReplaceAllUsages(Function &F, Function *Wrapper);
  void CreateWrapperBody(Function &F, Function *Wrapper);
  Function* RewriteVarargs(Function &F, IRBuilder<> &Builder, Value *&VAList, const SmallVector<VAStartInst*, 1> VAStarts);
  void MoveInstructionToWrapper(Instruction *I, BasicBlock *BB);
  void CreatePGLT(Module &M);
};
} // end anonymous namespace

char PGLTEntryWrappers::ID = 0;
INITIALIZE_PASS(PGLTEntryWrappers, "pglt-entry-wrappers",
                "PGLT Entry Wrappers", false, false)

ModulePass *llvm::createPGLTEntryWrappersPass() {
  return new PGLTEntryWrappers();
}

static bool SkipFunction(const Function &F) {
  return F.isDeclaration()
      || F.hasAvailableExternallyLinkage()
      || F.hasComdat()  // TODO: Support COMDAT
      || isa<UnreachableInst>(F.getEntryBlock().getTerminator());
      // Above condition is different from F.doesNotReturn(), which we do not
      // include (at least for now).
}

bool PGLTEntryWrappers::runOnModule(Module &M) {
  std::vector<Function*> Worklist;
  for (auto &F : M) {
    if (!SkipFunction(F)) Worklist.push_back(&F);
  }

  for (auto F : Worklist) {
    ProcessFunction(*F);
  }

  if (!Worklist.empty()) {
    CreatePGLT(M);
  }

  return !Worklist.empty();
}

static bool IsDirectCallOfBitcast(User *Usr) {
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(Usr)) {
    if (CE->getOpcode() == Instruction::BitCast) {
      // This bitcast must have exactly one user.
      // TODO(yln): we want to ensure that all uses of the bitcast are not skippable
      if (CE->user_begin() != CE->user_end()) {
        User *ParentUse = *CE->user_begin();
        // TOOD(yln): maybe recursive call to SkipFunctionUse
        if (CallInst *CI = dyn_cast<CallInst>(ParentUse)) {
          // TODO(yln): ImmutableCallSite
          CallSite CS(CI); // TODO(yln): also handle InvokeInst, create callsite with ParentUse, ask if valid CS
          // TODO(yln): add test with InvokeInst, confirm it points to wrapper, then make sure it gets optimized
//              if (CS)
          Use &CEU = *CE->use_begin();
          if (CS.isCallee(&CEU)) {
            return true;
          }
        }
        if (isa<GlobalAlias>(ParentUse))
          return true;
      }
    }
  }
  return false;
}

static bool SkipFunctionUse(const Use &U) {
  auto User = U.getUser();
  auto UserFn = dyn_cast<Function>(User);
  ImmutableCallSite CS(User);

  return (CS && CS.isCallee(&U))  // Used as the callee
      || isa<GlobalAlias>(User)   // No need to indirect
      || isa<BlockAddress>(User)  // Handled in AsmPrinter::EmitBasicBlockStart
      || (UserFn && UserFn->getPersonalityFn() == U.get()) // Skip pers. fn uses
      || IsDirectCallOfBitcast(User); // Calls to bitcasted functions end up as direct calls
}

void PGLTEntryWrappers::ProcessFunction(Function &F) {
  std::vector<Use*> AddressUses;
  for (Use &U : F.uses()) {
    if (!SkipFunctionUse(U)) AddressUses.push_back(&U);
  }

  if (!F.hasLocalLinkage() || !AddressUses.empty()) {
    CreateWrapper(F, AddressUses);
  }

  F.setSection("");
  F.addFnAttr(Attribute::RandPage);
}

static void ReplaceAddressTakenUse(Use *U, Function *F, Function *Wrapper, SmallSet<Constant*, 8> &Constants) {
  if (!U->get()) return; // Already replaced this use?

  if (auto GV = dyn_cast<GlobalVariable>(U->getUser())) {
    assert(GV->getInitializer() == F);
    GV->setInitializer(Wrapper);
  } else if (auto C = dyn_cast<Constant>(U->getUser())) {
    if (!Constants.count(C)) { // TODO(yln): I don't think this is needed
      Constants.insert(C);
      C->handleOperandChange(F, Wrapper); // Replace all uses at once
    }
  } else {
    U->set(Wrapper);
  }
}

void PGLTEntryWrappers::CreateWrapper(Function &F, const std::vector<Use*> &AddressUses) {
  Function *Wrapper = Function::Create(
      F.getFunctionType(), F.getLinkage(), F.getName() + WrapperSuffix, F.getParent());

  Wrapper->copyAttributesFrom(&F);
  Wrapper->setComdat(F.getComdat());
  // Ensure that the wrapper is not placed in an explicitly named section. If it
  // is, the section flags will be combined with other function in the section
  // (RandPage functions, potentially), and the wrapper will get marked
  // RAND_ADDR
  // TODO: Verify the above. This should not be the case unless functions are not
  // Wrapper->setSection("");

  // We can't put the wrapper function in an explicitly named section because
  // it then does not get a per-function section, which we need to properly
  // support --gc-sections
  // Wrapper->setSection(".text.wrappers");

  Wrapper->addFnAttr(Attribute::RandWrapper);
  //Wrapper->addFnAttr(Attribute::RandPage);  // TODO: SJC can we place wrappers on randomly located pages? I don't see why not, but this is safer for now
  Wrapper->addFnAttr(Attribute::NoInline);
  Wrapper->addFnAttr(Attribute::OptimizeForSize);

  CreateWrapperBody(F, Wrapper);

  // 1) Calls to a non-local function must go through the wrapper since they
  //    could be ridrected by the dynamic linker (i.e, LD_PRELOAD).
  // 2) Calls to vararg functions must go through the wrapper to ensure that we
  //    preserve the arguments on the stack when we indirect through the PGLT.
  // 3) Address-taken uses of local functions might escape, hence we must also
  //    replace them.
  if (!F.hasLocalLinkage() || F.isVarArg()) {
    ReplaceAllUsages(F, Wrapper);
  } else {
    assert(!AddressUses.empty());
    SmallSet<Constant*, 8> Constants; // TODO(yln): SmallPtrSetImpl without N=8
    for (auto U : AddressUses) {
      ReplaceAddressTakenUse(U, &F, Wrapper, Constants);
    }
  }
}

void PGLTEntryWrappers::ReplaceAllUsages(Function &F, Function *Wrapper) {
  std::string Name = F.getName() + (F.isVarArg() ? OrigVASuffix : OrigSuffix);
  Wrapper->takeName(&F);
  F.setName(Name);

  F.replaceAllUsesWith(Wrapper);

  if (!F.hasLocalLinkage()) {
    F.setVisibility(GlobalValue::HiddenVisibility);
  }
}

static SmallVector<VAStartInst*, 1> FindVAStarts(Function &F) {
  SmallVector<VAStartInst*, 1> Insts;
  for (auto &I : instructions(F)) {
    if (isa<VAStartInst>(&I)) {
      Insts.push_back(cast<VAStartInst>(&I));
    }
  }
  return Insts;
}

void PGLTEntryWrappers::CreateWrapperBody(Function &F, Function *Wrapper) {
  BasicBlock *BB = BasicBlock::Create(F.getContext(), "", Wrapper);
  IRBuilder<> Builder(BB);

  Value *VAList = nullptr;
  Function *DestFn = &F;
  if (F.isVarArg()) {
    auto VAStarts = FindVAStarts(F);
    if (!VAStarts.empty()) {
      DestFn = RewriteVarargs(F, Builder, VAList, VAStarts);
    }
  }

  // F may have been deleted at this point. DO NOT USE F!

  SmallVector<Value*, 8> Args;
  for (auto &A : Wrapper->args()) {
    Args.push_back(&A);
  }
  if (VAList) Args.push_back(VAList);

  CallInst *CI = Builder.CreateCall(DestFn, Args);
  CI->setCallingConv(Wrapper->getCallingConv());

  if (Wrapper->getReturnType()->isVoidTy()) {
    Builder.CreateRetVoid();
  } else {
    Builder.CreateRet(CI);
  }
}

static Instruction* findAlloca(Instruction* Use) {
  Instruction *Alloca = Use;
  while (Alloca && !isa<AllocaInst>(Alloca)) {
    Alloca = dyn_cast<Instruction>(Alloca->op_begin());
  }
  assert(Alloca && "Could not find va_list alloc in a var args functions");
  return Alloca;
}

Function* PGLTEntryWrappers::RewriteVarargs(Function &F, IRBuilder<> &Builder,
                                            Value *&VAList,
                                            const SmallVector<VAStartInst*, 1> VAStarts) {
  Module *M = F.getParent();
  FunctionType *FFTy = F.getFunctionType();

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
  Function* NewFn = Function::Create(NonVarArgs, F.getLinkage(), "", M);
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


  // TODO(yln): this can be removed, maybe
  // Set the PGLT base address
  auto PGLTAddress = M.getGlobalVariable("_PGLT_");
  if (!PGLTAddress) {
    PGLTAddress = new GlobalVariable(M, Type::getInt8Ty(C), true,
                                     GlobalValue::ExternalLinkage,
                                     nullptr, "_PGLT_");
    PGLTAddress->setVisibility(GlobalValue::ProtectedVisibility);
  }
}
