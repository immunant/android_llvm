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

  void processFunction(Function *F);
  Function *rewriteVarargs(Function &F, Type *&VAListTy);
  Function *createWrapper(Function &F, const SmallVectorImpl<Use *> &AddressUses);
  void createWrapperBody(Function *Wrapper, Function *Callee, Type *VAListTy);
};
} // end anonymous namespace

char PagerandoWrappers::ID = 0;
INITIALIZE_PASS(PagerandoWrappers, "pagerando-wrappers",
                "Pagerando entry wrappers", false, false)

ModulePass *llvm::createPagerandoWrappersPass() {
  return new PagerandoWrappers();
}

// We can safely skip functions consisting of only debug, trap, and unreachable
// instructions. Such functions are created for abstract, non-base
// destructors. We do not need to randomize these functions since they are
// trivial and not useful for an attacker to reuse.
static bool isTrivialFunction(const Function &F) {
  for (auto &I : F.getEntryBlock()) {
    if (isa<DbgInfoIntrinsic>(&I))
      continue;
    if (auto *Int = dyn_cast<IntrinsicInst>(&I))
      if (Int->getIntrinsicID() == Intrinsic::trap)
        continue;
    if (isa<UnreachableInst>(&I))
      continue;

    // We found an instruction that is not debug, trap, or unreachable.
    return false;
  }

  // We only found debug, trap, or unreachable instructions.
  return true;
}

static bool skipFunction(const Function &F) {
  return F.isDeclaration()
    || F.hasAvailableExternallyLinkage()
    || F.hasComdat() // TODO: Support COMDAT
    || isTrivialFunction(F);
}

bool PagerandoWrappers::runOnModule(Module &M) {
  std::vector<Function*> Worklist;
  for (auto &F : M) {
    if (!skipFunction(F))
      Worklist.push_back(&F);
  }

  for (auto F : Worklist)
    processFunction(F);

  return !Worklist.empty();
}

static bool skipFunctionUse(const Use &U);
static bool IsSkippableBitcast(User *Usr) {
  auto CE = dyn_cast<ConstantExpr>(Usr);
  return CE && CE->getOpcode() == Instruction::BitCast
      && std::all_of(CE->use_begin(), CE->use_end(), skipFunctionUse);
}

static User* getSingleUserOrNull(Value *V) {
  if (!V || !V->hasOneUse())
    return nullptr;

  return *V->user_begin();
}

static bool IsHiddenVTable(User *U) {
  // We want to verify that this use is a vtable initializer here. This relies
  // on the availability of TBAA to positively identify VTable uses, rather than
  // using a heuristic.
  //
  // We don't need to handle bitcasts because we have already stripped bitcast
  // exprs in IsSkippableBitcast.

  // We have one user (hopefully a vtable initializer)
  auto *CA = dyn_cast<ConstantArray>(U);

  // Struct containing the i8* array
  auto *CS = getSingleUserOrNull(CA);

  // Our user is an internal global
  auto *VTable = dyn_cast_or_null<GlobalValue>(getSingleUserOrNull(CS));
  if (!VTable || !VTable->hasLocalLinkage())
    return false;

  // All users of the vtable are marked as vtable accesses
  for (auto *VTUser : VTable->users()) {
    // Find the instruction using the (potential) VTable
    User *U = VTUser;
    while (U && !isa<Instruction>(U))
      U = getSingleUserOrNull(U);
    if (!U)
      return false;

    MDNode *Tag = cast<Instruction>(U)->getMetadata(LLVMContext::MD_tbaa);
    if (!Tag || !Tag->isTBAAVtableAccess())
      return false;
  }

  return true;
}

static bool skipFunctionUse(const Use &U) {
  auto User = U.getUser();
  auto UserFn = dyn_cast<Function>(User);
  ImmutableCallSite CS(User);

  return (CS && CS.isCallee(&U))  // Used as the callee
      || isa<GlobalAlias>(User)   // No need to indirect
      || isa<BlockAddress>(User)  // Handled in AsmPrinter::EmitBasicBlockStart
      || (UserFn && UserFn->getPersonalityFn() == U.get()) // Skip pers. fn uses
      || IsSkippableBitcast(User) // Calls to bitcasted functions end up as direct calls
      || IsHiddenVTable(User);
}

void PagerandoWrappers::processFunction(Function *F) {
  SmallVector<Use*, 4> AddressUses;
  for (Use &U : F->uses()) {
    if (!skipFunctionUse(U))
      AddressUses.push_back(&U);
  }

  if (!F->hasLocalLinkage() || !AddressUses.empty()) {
    auto Wrapper = createWrapper(*F, AddressUses);
    Type *VAListTy = nullptr;
    if (F->isVarArg()) {
      // Reassign F, it might have been deleted
      F = rewriteVarargs(*F, /* out */ VAListTy);
    }
    createWrapperBody(Wrapper, F, VAListTy);
  }

  F->setSection("");
  F->addFnAttr(Attribute::Pagerando);
}

static void replaceAddressTakenUse(Use *U, Function *F, Function *Wrapper,
                                   SmallSet<Constant*, 8> &Constants) {
  // Already replaced this use?
  if (!U->get()) return;

  if (auto GV = dyn_cast<GlobalVariable>(U->getUser())) {
    assert(GV->getInitializer() == F);
    GV->setInitializer(Wrapper);
  } else if (auto C = dyn_cast<Constant>(U->getUser())) {
    // Constant::handleOperandChange must not be called more than once per user
    if (Constants.insert(C).second)
      C->handleOperandChange(F, Wrapper);
  } else {
    U->set(Wrapper);
  }
}

Function *PagerandoWrappers::createWrapper(Function &F,
                                           const SmallVectorImpl<Use *> &AddressUses) {
  std::string OriginalName = F.getName();
  F.setName(Twine(OriginalName, F.isVarArg() ? OrigVASuffix : OrigSuffix));

  auto Wrapper = Function::Create(F.getFunctionType(), F.getLinkage(), OriginalName,
                                  F.getParent());
  Wrapper->copyAttributesFrom(&F);
  Wrapper->setComdat(F.getComdat());

  Wrapper->addFnAttr(Attribute::NoInline);
  Wrapper->addFnAttr(Attribute::OptimizeForSize);

  // +) Calls to a non-local function must go through the wrapper since they
  //    could be redirected by the dynamic linker (i.e, LD_PRELOAD).
  // +) Calls to vararg functions must go through the wrapper to ensure that we
  //    preserve the arguments on the stack when we indirect through the POT.
  // -) Address-taken uses of local functions might escape, hence we must also
  //    replace them.
  if (!F.hasLocalLinkage() || F.isVarArg()) {
    F.replaceAllUsesWith(Wrapper);
    if (!F.hasLocalLinkage())
      F.setVisibility(GlobalValue::HiddenVisibility);
  } else {
    assert(!AddressUses.empty());
    SmallSet<Constant*, 8> Constants;
    for (auto U : AddressUses)
      replaceAddressTakenUse(U, &F, Wrapper, Constants);
  }

  return Wrapper;
}

static SmallVector<VAStartInst*, 1> findVAStarts(Function &F) {
  SmallVector<VAStartInst*, 1> Insts;
  for (auto &I : instructions(F)) {
    if (isa<VAStartInst>(&I))
      Insts.push_back(cast<VAStartInst>(&I));
  }
  return Insts;
}

static AllocaInst *findAlloca(VAStartInst *VAStart) {
  Instruction *Inst = VAStart;
  while (Inst && !isa<AllocaInst>(Inst))
    Inst = dyn_cast<Instruction>(Inst->op_begin());

  assert(Inst && "Could not find va_list alloca in var args function");
  return cast<AllocaInst>(Inst);
}

static AllocaInst *createVAList(Module *M, IRBuilder<> &Builder, Type *VAListTy) {
  auto VAListAlloca = Builder.CreateAlloca(VAListTy);
  Builder.CreateCall(  // @llvm.va_start(i8* <arglist>)
      Intrinsic::getDeclaration(M, Intrinsic::vastart),
      { Builder.CreateBitCast(VAListAlloca, Builder.getInt8PtrTy()) });

  return VAListAlloca;
}

static void createVAEndCall(IRBuilder<> &Builder, AllocaInst *VAListAlloca) {
  Builder.CreateCall(  // @llvm.va_end(i8* <arglist>)
      Intrinsic::getDeclaration(VAListAlloca->getModule(), Intrinsic::vaend),
      { Builder.CreateBitCast(VAListAlloca, Builder.getInt8PtrTy()) });
}

static void replaceWithVACopyCall(IRBuilder<> &Builder, VAStartInst *VAStart,
                                  Argument *VAListArg) {
  Builder.SetInsertPoint(VAStart);
  Builder.CreateCall(  // @llvm.va_copy(i8* <destarglist>, i8* <srcarglist>)
      Intrinsic::getDeclaration(VAStart->getModule(), Intrinsic::vacopy),
      { VAStart->getArgOperand(0),
          Builder.CreateBitCast(VAListArg, Builder.getInt8PtrTy()) });
  VAStart->eraseFromParent();
}

void PagerandoWrappers::createWrapperBody(Function *Wrapper, Function *Callee,
                                          Type *VAListTy) {
  auto BB = BasicBlock::Create(Wrapper->getContext(), "", Wrapper);
  IRBuilder<> Builder(BB);

  // Arguments
  SmallVector<Value*, 8> Args;
  for (auto &A : Wrapper->args())
    Args.push_back(&A);

  // va_list alloca and va_start
  AllocaInst* VAListAlloca;
  if (VAListTy) {
    VAListAlloca = createVAList(Wrapper->getParent(), Builder, VAListTy);
    Args.push_back(VAListAlloca);
  }

  // Call
  auto Call = Builder.CreateCall(Callee, Args);
  Call->setCallingConv(Callee->getCallingConv());

  // va_end
  if (VAListTy)
    createVAEndCall(Builder, VAListAlloca);

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
    ++DestArg;
  }

  // Replace va_start with va_copy
  IRBuilder<> Builder(NF->getContext());
  auto VAListArg = NF->arg_end() - 1;
  for (auto VAStart : VAStarts)
    replaceWithVACopyCall(Builder, VAStart, VAListArg);

  // Delete original function
  F.eraseFromParent();

  return NF;
}
