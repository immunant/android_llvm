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

// SkipTrivialWrappers - Do not wrap trivial functions that are only a call to
// another function and a return.
static cl::opt<bool> SkipTrivialWrappers(
    "pagerando-skip-trivial",
    cl::desc("Do not apply pagerando to wrapper functions consisting of only a single call."),
    cl::Hidden, cl::init(false));

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
//
// We may want to skip functions that consist of only a single call and a
// return. Wrapping these functions for pagerando introduces a proportionally
// larger overhead than for functions with non-trivial bodies. Reusing the
// content of such a function is equivalent to reusing the whole function, since
// the content is only a single function call (modulo any move operations to get
// arguments into the right order).
static bool isTrivialFunction(const Function &F, bool singleCallTrivial = false) {
  bool sawCall = false;
  for (auto &I : F.getEntryBlock()) {
    if (isa<DbgInfoIntrinsic>(&I))
      continue;
    if (auto *Int = dyn_cast<IntrinsicInst>(&I))
      if (Int->getIntrinsicID() == Intrinsic::trap)
        continue;
    if (isa<UnreachableInst>(&I))
      continue;
    if (singleCallTrivial) {
      auto *CI = dyn_cast<CallInst>(&I);
      // We cannot call a binned function via a tail-call if we need to load the
      // POT register, since the POT register is callee-saved and must be
      // restored after the call.
      if (!sawCall && CI && !CI->isMustTailCall()) {
        sawCall = true;
        continue;
      }
      if (isa<ReturnInst>(&I))
        continue;
    }

    // We found an instruction that is not debug, trap, or unreachable.
    return false;
  }

  // We only found debug, trap, or unreachable instructions.
  return true;
}

// We skip functions that are only declarations, comdat, trivial trap functions,
// and naked functions. Skipping naked functions is important so that CFI jump
// tables are not placed in pagerando sections. CFI jump tables are marked as
// naked in LowerTypeTests::createJumpTable. If this ever changes, this function
// will also need to be updated.
//
// TODO: Support COMDAT
static bool skipFunction(const Function &F) {
  return F.isDeclaration()
    || F.hasAvailableExternallyLinkage()
    || F.hasComdat()
    || isTrivialFunction(F, SkipTrivialWrappers)
    || F.hasFnAttribute(Attribute::Naked)
    || F.hasFnAttribute("thunk");
}

bool PagerandoWrappers::runOnModule(Module &M) {
  std::vector<Function*> Worklist;
  for (auto &F : M) {
    if (!F.hasFnAttribute(Attribute::Pagerando))
      continue;
    if (skipFunction(F)) {
      F.removeFnAttr(Attribute::Pagerando);
      continue;
    }
    Worklist.push_back(&F);
  }

  for (auto F : Worklist)
    processFunction(F);

  return !Worklist.empty();
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
      || isa<BlockAddress>(User)  // Handled in AsmPrinter::EmitBasicBlockStart
      || (UserFn && UserFn->getPersonalityFn() == U.get()) // Skip pers. fn uses
      || IsDirectCallOfBitcast(User); // Calls to bitcasted functions end up as direct calls
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
}

// Replace F with Wrapper when applicable according to the following rules:
// - Calls to vararg functions must always go through the wrapper to ensure that
//   we preserve the arguments on the stack when we indirect through the POT.
// - Calls to a non-local, non-protected function must go through the wrapper
//   since they could be redirected by the dynamic linker (i.e, LD_PRELOAD).
// - Calls to protected visibility functions do not need to go through a wrapper
//   since protected functions cannot be preempted at load time.
// - Address-taken uses of local functions might escape, so we must replace
//   these addresses with the address of a wrapper.
static void replaceWithWrapper(Function &F, Function *Wrapper,
                        const SmallVectorImpl<Use *> &AddressUses) {
  if (F.isVarArg() || (!F.hasLocalLinkage() && !F.hasProtectedVisibility())) {
    F.replaceAllUsesWith(Wrapper);
    if (!F.hasLocalLinkage())
      F.setVisibility(GlobalValue::ProtectedVisibility);
  } else {
    // Replace all address-taken uses so we don't leak a binned address to
    // another DSO.
    SmallSet<Constant*, 8> Constants;
    for (auto U : AddressUses) {
      // Already replaced this use?
      if (!U->get())
        continue;

      if (auto GV = dyn_cast<GlobalVariable>(U->getUser())) {
        assert(GV->getInitializer() == &F);
        GV->setInitializer(Wrapper);
      } else if (auto C = dyn_cast<Constant>(U->getUser())) {
        // Constant::handleOperandChange must not be called more than once per user
        if (Constants.insert(C).second) {
          // Aliases cannot handle operand change, so we need to change the
          // aliasee directly.
          if (auto *GA = dyn_cast<GlobalAlias>(C)) {
            assert(GA->getAliasee() == &F);
            GA->setAliasee(Wrapper);
          } else {
            C->handleOperandChange(&F, Wrapper);
          }
        }
      } else {
        U->set(Wrapper);
      }
    }
  }
}

Function *PagerandoWrappers::createWrapper(Function &F,
                                           const SmallVectorImpl<Use *> &AddressUses) {
  std::string OriginalName = F.getName();
  F.setName(Twine(OriginalName, F.isVarArg() ? OrigVASuffix : OrigSuffix));

  // We add the function to the module explicitly because we want to control
  // ordering. The new wrapper should go at the beginning of the module to
  // ensure that at least one function in the normal text section will be
  // emitted first. Without this, the linker may try to place a bin at the
  // beginning of the file instead of the normal text section.
  auto Wrapper = Function::Create(F.getFunctionType(), F.getLinkage(), OriginalName);
  F.getParent()->getFunctionList().push_front(Wrapper);
  Wrapper->setComdat(F.getComdat());

  // Copy all attributes, then reset and explicitly remove blacklisted
  // attributes in various categories. This saves us from having to maintain all
  // explicit parameters here and in copyAttributesFrom.
  Wrapper->copyAttributesFrom(&F);

  // Wrappers cannot throw, so we don't need a personality function
  Wrapper->setPersonalityFn(nullptr);

  // Blacklist target independent attributes that should not be inherited
  for (const auto &Attr : F.getAttributes().getFnAttributes()) {
    if (Attr.isStringAttribute()) {
      // We want wrappers to be as small as possible, so we want to eliminate
      // the frame pointer if possible.
      auto AttrString = Attr.getKindAsString();
      if (AttrString == "no-frame-pointer-elim" ||
          AttrString == "no-frame-pointer-elim-non-leaf")
        Wrapper->removeFnAttr(Attr.getKindAsString());

      continue;
    }

    switch (Attr.getKindAsEnum()) {
    // These attributes cannot be propagated safely. Explicitly list them here
    // so we get a warning if new attributes are added. This list also includes
    // non-function attributes.
    case Attribute::Alignment:
    case Attribute::AlwaysInline:
    case Attribute::ArgMemOnly:
    case Attribute::Builtin:
    case Attribute::ByVal:
    case Attribute::Dereferenceable:
    case Attribute::DereferenceableOrNull:
    case Attribute::InAlloca:
    case Attribute::InReg:
    case Attribute::InlineHint:
    case Attribute::MinSize:
    case Attribute::Naked:
    case Attribute::Nest:
    case Attribute::NoAlias:
    case Attribute::NoCapture:
    case Attribute::NoInline:
    case Attribute::NoRedZone:
    case Attribute::NonNull:
    case Attribute::None:
    case Attribute::OptimizeForSize:
    case Attribute::OptimizeNone:
    case Attribute::Pagerando:
    case Attribute::ReadNone:
    case Attribute::Returned:
    case Attribute::ReturnsTwice:
    case Attribute::SExt:
    case Attribute::StructRet:
    case Attribute::SwiftError:
    case Attribute::SwiftSelf:
    case Attribute::WriteOnly:
    case Attribute::ZExt:
    case Attribute::EndAttrKinds:
      break;
    // These attributes should be safe to propagate to the wrapper function
    case Attribute::AllocSize:
    case Attribute::Cold:
    case Attribute::Convergent:
    case Attribute::InaccessibleMemOnly:
    case Attribute::InaccessibleMemOrArgMemOnly:
    case Attribute::JumpTable:
    case Attribute::NoBuiltin:
    case Attribute::NoDuplicate:
    case Attribute::NoImplicitFloat:
    case Attribute::NoRecurse:
    case Attribute::NoReturn:
    case Attribute::NoUnwind:
    case Attribute::NonLazyBind:
    case Attribute::ReadOnly:
    case Attribute::SafeStack:
    case Attribute::SanitizeAddress:
    case Attribute::SanitizeHWAddress:
    case Attribute::SanitizeMemory:
    case Attribute::SanitizeThread:
    case Attribute::Speculatable:
    case Attribute::StackAlignment:
    case Attribute::StackProtectReq:
    case Attribute::StrictFP:
    case Attribute::UWTable:
      continue;
    // These attributes should be propagated iff F is varargs (i.e. we need an
    // Alloca)
    case Attribute::StackProtect:
    case Attribute::StackProtectStrong:
      if (!F.isVarArg())
        break;
      continue;
    }

    Wrapper->removeFnAttr(Attr.getKindAsEnum());
  }

  Wrapper->addFnAttr(Attribute::NoInline);
  Wrapper->addFnAttr(Attribute::OptimizeForSize);

  replaceWithWrapper(F, Wrapper, AddressUses);

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
