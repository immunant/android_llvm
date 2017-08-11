//===-- PagerandoWrappers.cpp - Pagerando entry wrappers ------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass creates wrappers for pagerando-enabled functions. A function needs
// a wrapper if it has non-local linkage or its address taken, i.e., if it can
// be used from outside the module. (As an optimization we could use pointer
// escape analysis for address-taken functions instead of creating wrappers for
// all of them.)
// Vararg functions require special treatment: their variable arguments on the
// stack need to be preserved even when indirecting through the POT. We replace
// the original function with a new function that takes an explicit 'va_list'
// parameter:  foo(int, ...) -> foo$$origva(int, *va_list). The wrapper captures
// its variable arguments and explicitly passes it to the adapted function to
// preserve the variable arguments passed by the caller.
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
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

#define DEBUG_TYPE "pagerando"

namespace {
class PagerandoWrappers : public ModulePass {
public:
  static char ID;
  explicit PagerandoWrappers() : ModulePass(ID) {
    initializePagerandoWrappersPass(*PassRegistry::getPassRegistry());
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

  void processFunction(Function *F);
  Function *rewriteVarargs(Function &F, Type *&VAListTy);
  Function *createWrapper(Function &F, const std::vector<Use *> &AddressUses);
  void createWrapperBody(Function *Wrapper, Function *Callee, Type *VAListTy);
  void createPOT(Module &M);
};
} // end anonymous namespace

char PagerandoWrappers::ID = 0;
INITIALIZE_PASS(PagerandoWrappers, "pagerando-wrappers",
                "Pagerando entry wrappers", false, false)

ModulePass *llvm::createPagerandoWrappersPass() {
  return new PagerandoWrappers();
}

static bool skipFunction(const Function &F) {
  return F.isDeclaration()
      || F.hasAvailableExternallyLinkage()
      || F.hasComdat()  // TODO: Support COMDAT
      || isa<UnreachableInst>(F.getEntryBlock().getTerminator());
      // Above condition is different from F.doesNotReturn(), which we do not
      // include (at least for now).
}

bool PagerandoWrappers::runOnModule(Module &M) {
  std::vector<Function*> Worklist;
  for (auto &F : M) {
    if (!skipFunction(F)) Worklist.push_back(&F);
  }

  if (Worklist.empty()) {
    return false;
  }

  for (auto F : Worklist) {
    processFunction(F);
  }
  createPOT(M);

  return true;
}

static bool skipFunctionUse(const Use &U);
static bool IsDirectCallOfBitcast(User *Usr) {
  auto CE = dyn_cast<ConstantExpr>(Usr);
  return CE && CE->getOpcode() == Instruction::BitCast
      && std::all_of(CE->use_begin(), CE->use_end(), skipFunctionUse);
}

static bool skipFunctionUse(const Use &U) {
  auto User = U.getUser();
  auto UserFn = dyn_cast<Function>(User);
  ImmutableCallSite CS(User);

  return (CS && CS.isCallee(&U))  // Used as the callee
      || isa<GlobalAlias>(User)   // No need to indirect
      || isa<BlockAddress>(User)  // Handled in AsmPrinter::EmitBasicBlockStart
      || (UserFn && UserFn->getPersonalityFn() == U.get()) // Skip pers. fn uses
      || IsDirectCallOfBitcast(User); // Calls to bitcasted functions end up as direct calls
}

void PagerandoWrappers::processFunction(Function *F) {
  std::vector<Use*> AddressUses;
  for (Use &U : F->uses()) {
    if (!skipFunctionUse(U)) AddressUses.push_back(&U);
  }

  if (!F->hasLocalLinkage() || !AddressUses.empty()) {
    auto Wrapper = createWrapper(*F, AddressUses);
    Type *VAListTy = nullptr;
    if (F->isVarArg()) {
      F = rewriteVarargs(*F, /* out */ VAListTy); // Reassign F, it might have been deleted
    }
    createWrapperBody(Wrapper, F, VAListTy);
  }

  F->setSection("");
  F->addFnAttr(Attribute::PagerandoBinned);
}

static void replaceAddressTakenUse(Use *U, Function *F, Function *Wrapper,
                                   SmallSet<Constant*, 8> &Constants) {
  if (!U->get()) return; // Already replaced this use?

  if (auto GV = dyn_cast<GlobalVariable>(U->getUser())) {
    assert(GV->getInitializer() == F);
    GV->setInitializer(Wrapper);
  } else if (auto C = dyn_cast<Constant>(U->getUser())) {
    if (Constants.insert(C).second) {     // Constant::handleOperandChange must
      C->handleOperandChange(F, Wrapper); // not be called more than once per user
    }
  } else {
    U->set(Wrapper);
  }
}

Function *PagerandoWrappers::createWrapper(Function &F,
                                           const std::vector<Use *> &AddressUses) {
  auto Wrapper = Function::Create(F.getFunctionType(), F.getLinkage(),
                                  F.getName() + WrapperSuffix, F.getParent());
  Wrapper->copyAttributesFrom(&F);
  Wrapper->setComdat(F.getComdat());

  Wrapper->addFnAttr(Attribute::PagerandoWrapper);
  //Wrapper->addFnAttr(Attribute::PagerandoBinned);  // TODO: SJC can we place wrappers on randomly located pages? I don't see why not, but this is safer for now
  Wrapper->addFnAttr(Attribute::NoInline);
  Wrapper->addFnAttr(Attribute::OptimizeForSize);

  // +) Calls to a non-local function must go through the wrapper since they
  //    could be redirected by the dynamic linker (i.e, LD_PRELOAD).
  // +) Calls to vararg functions must go through the wrapper to ensure that we
  //    preserve the arguments on the stack when we indirect through the POT.
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
    SmallSet<Constant*, 8> Constants;
    for (auto U : AddressUses) {
      replaceAddressTakenUse(U, &F, Wrapper, Constants);
    }
  }

  return Wrapper;
}

static SmallVector<VAStartInst*, 1> findVAStarts(Function &F) {
  SmallVector<VAStartInst*, 1> Insts;
  for (auto &I : instructions(F)) {
    if (isa<VAStartInst>(&I)) {
      Insts.push_back(cast<VAStartInst>(&I));
    }
  }
  return Insts;
}

static AllocaInst *findAlloca(VAStartInst *VAStart) {
  Instruction *Alloca = VAStart;
  while (Alloca && !isa<AllocaInst>(Alloca)) {
    Alloca = dyn_cast<Instruction>(Alloca->op_begin());
  }
  assert(Alloca && "Could not find va_list alloca in var args function");
  return cast<AllocaInst>(Alloca);
}

static AllocaInst* createVAList(Module *M, IRBuilder<> &Builder, Type *VAListTy) {
  auto VAListAlloca = Builder.CreateAlloca(VAListTy);
  Builder.CreateCall(  // @llvm.va_start(i8* <arglist>)
      Intrinsic::getDeclaration(M, Intrinsic::vastart),
      {Builder.CreateBitCast(VAListAlloca, Builder.getInt8PtrTy())});

  return VAListAlloca;
}

static void createVACopyCall(IRBuilder<> &Builder, VAStartInst *VAStart,
                             Argument *VAListArg) {
  Builder.SetInsertPoint(VAStart);
  Builder.CreateCall(  // @llvm.va_copy(i8* <destarglist>, i8* <srcarglist>)
      Intrinsic::getDeclaration(VAStart->getModule(), Intrinsic::vacopy),
      {VAStart->getArgOperand(0),
       Builder.CreateBitCast(VAListArg, Builder.getInt8PtrTy())});
}

void PagerandoWrappers::createWrapperBody(Function *Wrapper, Function *Callee,
                                          Type *VAListTy) {
  auto BB = BasicBlock::Create(Wrapper->getContext(), "", Wrapper);
  IRBuilder<> Builder(BB);

  // Arguments
  SmallVector<Value*, 8> Args;
  for (auto &A : Wrapper->args()) {
    Args.push_back(&A);
  }
  if (VAListTy) {
    auto VAListAlloca = createVAList(Wrapper->getParent(), Builder, VAListTy);
    Args.push_back(VAListAlloca);
  }

  // Call
  auto Call = Builder.CreateCall(Callee, Args);
  Call->setCallingConv(Callee->getCallingConv());

  // Return
  if (Wrapper->getReturnType()->isVoidTy()) {
    Builder.CreateRetVoid();
  } else {
    Builder.CreateRet(Call);
  }
}

// Replaces the original function with a new function that takes a va_list
// parameter but is not varargs:  foo(int, ...) -> foo$$origva(int, *va_list)
Function *PagerandoWrappers::rewriteVarargs(Function &F, Type *&VAListTy) {
  auto VAStarts = findVAStarts(F);
  if (VAStarts.empty()) return &F;

  // Determine va_list type
  auto VAListAlloca = findAlloca(VAStarts[0]);
  VAListTy = VAListAlloca->getAllocatedType();

  // Adapt function type
  auto FTy = F.getFunctionType();
  SmallVector<Type*, 8> Params(FTy->param_begin(), FTy->param_end());
  Params.push_back(VAListTy->getPointerTo());
  auto NonVAFty = FunctionType::get(FTy->getReturnType(), Params, false);

  // Create new function definition
  auto NF = Function::Create(NonVAFty, F.getLinkage(), "", F.getParent());
  NF->takeName(&F);
  NF->copyAttributesFrom(&F);
  NF->setComdat(F.getComdat());
  NF->setSubprogram(F.getSubprogram());

  // Move basic blocks into new function; F is now dysfunctional
  NF->getBasicBlockList().splice(NF->begin(), F.getBasicBlockList());

  // Adapt arguments (FN's additional 'va_list' arg does not need adaption)
  auto DestArg = NF->arg_begin();
  for (auto &A : F.args()) {
    A.replaceAllUsesWith(DestArg);
    DestArg->takeName(&A);
    DestArg++;
  }

  // Adapt va_list uses
  auto VAListArg = NF->arg_end() - 1;

  // +) For a single va_start call we can remove the va_list alloca and
  //    va_start, and use the parameter directly instead.
  // -) For more than one va_start we need to keep the va_list alloca and
  //    replace va_start with a va_copy.
  if (VAStarts.size() == 1) {
    VAListAlloca->replaceAllUsesWith(VAListArg);
    VAListAlloca->eraseFromParent();
  } else {
    IRBuilder<> Builder(NF->getContext());
    for (auto VAStart : VAStarts) {
      createVACopyCall(Builder, VAStart, VAListArg);
    }
  }
  for (auto VAStart : VAStarts) VAStart->eraseFromParent();

  // Delete original function
  F.eraseFromParent();

  return NF;
}

void PagerandoWrappers::createPOT(Module &M) {
  auto PtrTy = Type::getInt8PtrTy(M.getContext());
  auto Ty = ArrayType::get(PtrTy, /* NumElements */ 1);
  auto Init = ConstantAggregateZero::get(Ty);
  auto POT = new GlobalVariable(
      M, Ty, /* constant */ true, GlobalValue::ExternalLinkage, Init, "llvm.pot");
  POT->setVisibility(GlobalValue::ProtectedVisibility);

  llvm::appendToUsed(M, {POT});
}
