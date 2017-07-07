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

  void ProcessFunction(Function *F);
  Function *RewriteVarargs(Function &F);
  Function *CreateWrapper(Function &F, const std::vector<Use*> &AddressUses);
  void CreateWrapperBody(Function *Wrapper, Function* F, bool VARewritten);
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
    ProcessFunction(F);
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

void PGLTEntryWrappers::ProcessFunction(Function *F) {
  std::vector<Use*> AddressUses;
  for (Use &U : F->uses()) {
    if (!SkipFunctionUse(U)) AddressUses.push_back(&U);
  }

  if (!F->hasLocalLinkage() || !AddressUses.empty()) {
    auto Wrapper = CreateWrapper(*F, AddressUses);
    bool VARewritten = false;
    if (F->isVarArg()) {
      auto NF = RewriteVarargs(*F);
      VARewritten = NF != F;
      F = NF;   // Reassign F because it might have been deleted
    }
    CreateWrapperBody(Wrapper, F, VARewritten);
  }

  F->setSection("");
  F->addFnAttr(Attribute::RandPage);
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

Function *PGLTEntryWrappers::CreateWrapper(Function &F, const std::vector<Use*> &AddressUses) {
  auto Wrapper = Function::Create(F.getFunctionType(), F.getLinkage(),
                                  F.getName() + WrapperSuffix, F.getParent());
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

  // +) Calls to a non-local function must go through the wrapper since they
  //    could be redirected by the dynamic linker (i.e, LD_PRELOAD).
  // +) Calls to vararg functions must go through the wrapper to ensure that we
  //    preserve the arguments on the stack when we indirect through the PGLT.
  // -) Address-taken uses of local functions might escape, hence we must also
  //    replace them.
  if (!F.hasLocalLinkage() || F.isVarArg()) {
    // Take name, replace usages, hide original function
    std::string OldName = F.getName();
    Wrapper->takeName(&F);
    F.setName(OldName + (F.isVarArg() ? OrigVASuffix : OrigSuffix));
    F.replaceAllUsesWith(Wrapper);
    if (!F.hasLocalLinkage()) {
      F.setVisibility(GlobalValue::HiddenVisibility);
    }
  } else {
    assert(!AddressUses.empty());
    SmallSet<Constant*, 8> Constants; // TODO(yln): SmallPtrSetImpl without N=8
    for (auto U : AddressUses) {
      ReplaceAddressTakenUse(U, &F, Wrapper, Constants);
    }
  }

  return Wrapper;
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

static AllocaInst *FindAlloca(VAStartInst *VAStart) {
  Instruction *Alloca = VAStart;
  while (Alloca && !isa<AllocaInst>(Alloca)) {
    Alloca = dyn_cast<Instruction>(Alloca->op_begin());
  }
  assert(Alloca && "Could not find va_list alloca in var args function");
  return cast<AllocaInst>(Alloca);
}

// TODO(yln): brittle, think about better way
// Should we create it ourselves? va_list is a platform-dependent type
// From LLVM docs:
//; This struct is different for every platform. For most platforms,
//; it is merely an i8*.
//%struct.va_list = type { i8* }
//
//; For Unix x86_64 platforms, va_list is the following struct:
//; %struct.va_list = type { i32, i32, i8*, i8* }
static StructType *GetVAListType(const Module *M) {
  constexpr const char *VAListTyNames[] = {"struct.__va_list",
                                           "struct.std::__va_list"};
  for (auto TyName : VAListTyNames) {
    auto Ty = M->getTypeByName(TyName);
    if (Ty) return Ty;
  }
  llvm_unreachable("Could not retrieve 'va_list' type");
}

static AllocaInst* CreateVAList(Module *M, IRBuilder<> &Builder) {
  auto VAListAlloca = Builder.CreateAlloca(GetVAListType(M));
  Builder.CreateCall(  // @llvm.va_start(i8* <arglist>)
      Intrinsic::getDeclaration(M, Intrinsic::vastart),
      {Builder.CreateBitCast(VAListAlloca, Builder.getInt8PtrTy())});

  return VAListAlloca;
}

static void CreateVACopyCall(IRBuilder<> &Builder, VAStartInst *VAStart, Argument *VAListArg) {
  Builder.SetInsertPoint(VAStart);
  Builder.CreateCall(  // @llvm.va_copy(i8* <destarglist>, i8* <srcarglist>)
      Intrinsic::getDeclaration(VAStart->getModule(), Intrinsic::vacopy),
      {VAStart->getArgOperand(0),
       Builder.CreateBitCast(VAListArg, Builder.getInt8PtrTy())});
}

void PGLTEntryWrappers::CreateWrapperBody(Function *Wrapper, Function *F, bool VARewritten) {
  auto BB = BasicBlock::Create(Wrapper->getContext(), "", Wrapper);
  IRBuilder<> Builder(BB);

  // Arguments
  SmallVector<Value*, 8> Args;
  for (auto &A : Wrapper->args()) {
    Args.push_back(&A);
  }
  if (VARewritten) {
    auto VAListAlloca = CreateVAList(Wrapper->getParent(), Builder);
    Args.push_back(VAListAlloca);
  }

  // Call
  auto CI = Builder.CreateCall(F, Args);
  CI->setCallingConv(Wrapper->getCallingConv());

  // Return
  if (Wrapper->getReturnType()->isVoidTy()) {
    Builder.CreateRetVoid();
  } else {
    Builder.CreateRet(CI);
  }
}

// Creates a new function that takes a va_list parameter but is not varargs
Function *PGLTEntryWrappers::RewriteVarargs(Function &F) {
  auto VAStarts = FindVAStarts(F);
  if (VAStarts.empty()) return &F;

  // Adapt function type
  auto M = F.getParent();
  auto FTy = F.getFunctionType();
  SmallVector<Type*, 8> Params(FTy->param_begin(), FTy->param_end());
  Params.push_back(GetVAListType(M)->getPointerTo());
  auto NonVAFty = FunctionType::get(FTy->getReturnType(), Params, false);

  // Create new function definition
  auto Dest = Function::Create(NonVAFty, F.getLinkage(), "", M);
  Dest->takeName(&F);
  Dest->copyAttributesFrom(&F);
  Dest->setComdat(F.getComdat());
  Dest->setSubprogram(F.getSubprogram());
//  Dest->copyMetadata(&F, 1337); // TODO(yln): what happens to metadata?

//  Dest->stealArgumentListFrom(Src); // TODO(yln): Try this instead of adapting args, but probably doesn't work since it is one arg shorter
  // Move basic blocks into new function; F is now dysfunctional
  Dest->getBasicBlockList().splice(Dest->begin(), F.getBasicBlockList());

  // Adapt arguments (NewFn's additional 'va_list' arg does not need adaption)
  auto DestArg = Dest->arg_begin();
  for (auto &A : F.args()) {
    A.replaceAllUsesWith(DestArg);
    DestArg->takeName(&A);
    DestArg++;
  }

  // Adapt va_list uses
  auto VAListArg = Dest->arg_end() - 1;

  // +) For a single va_start call we can remove the va_list alloca and
  //    va_start, and use the parameter directly instead.
  // -) For more than one va_start we need to keep the va_list alloca and
  //    replace va_start with a va_copy.
  if (VAStarts.size() == 1) {
    auto VAListAlloca = FindAlloca(VAStarts[0]);
    VAListAlloca->replaceAllUsesWith(VAListArg);
    VAListAlloca->eraseFromParent();
  } else {
    IRBuilder<> Builder(Dest->getContext());
    for (auto VAStart : VAStarts) {
      CreateVACopyCall(Builder, VAStart, VAListArg);
    }
  }
  for (auto VAStart : VAStarts) VAStart->eraseFromParent();

  // Delete original function
  F.eraseFromParent();

  return Dest;
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


  // TODO(yln): this can be removed, maybe, alternatively remove some redundant code in AsmPrinter::EmitPGLT
  // Set the PGLT base address
  auto PGLTAddress = M.getGlobalVariable("_PGLT_");
  if (!PGLTAddress) {
    PGLTAddress = new GlobalVariable(M, Type::getInt8Ty(C), true,
                                     GlobalValue::ExternalLinkage,
                                     nullptr, "_PGLT_");
    PGLTAddress->setVisibility(GlobalValue::ProtectedVisibility);
  }
}
