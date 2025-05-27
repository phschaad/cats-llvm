#include "cats_passes.hpp"

#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include "../runtime/cats_runtime.h"

using namespace llvm;

void instrumentExit(IRBuilder<> &Builder, FunctionCallee ExitFunc,
                    Constant *ScopeID, Constant *FuncNamePtr,
                    Constant *FilenamePtr, Instruction *Inst) {
  // Get debug location information
  const DebugLoc &DL = Inst->getDebugLoc();
  unsigned Line = 0;
  unsigned Col = 0;

  if (DL) {
    Line = DL.getLine();
    Col = DL.getCol();
  }

  LLVMContext &Context = Builder.getContext();
  Value *ExitArgs[] = {
      ConstantInt::get(Type::getInt64Ty(Context), g_cats_instrument_call_id++),
      ScopeID,
      FuncNamePtr,
      FilenamePtr,
      ConstantInt::get(Type::getInt32Ty(Context), Line),
      ConstantInt::get(Type::getInt32Ty(Context), Col)};
  Builder.CreateCall(ExitFunc, ExitArgs);
}

PreservedAnalyses FunctionScopeTrackerPass::run(
  Function &F, FunctionAnalysisManager &AM
) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();

  if (functionHasAnnotation(F, "cats_noinstrument")) {
    errs() << "Skipping function " << F.getName() << "\n";
    return PreservedAnalyses::all();
  }

  bool Modified = false;

  Module *M = F.getParent();
  LLVMContext &Context = M->getContext();

  // Get or create the instrument functions
  FunctionCallee EnterFunc = M->getOrInsertFunction(
    "cats_trace_instrument_scope_entry",
    FunctionType::get(Type::getVoidTy(Context),
                      {Type::getInt64Ty(Context),       /*call_id*/
                       Type::getInt32Ty(Context),       /*scope_id*/
                       Type::getInt8Ty(Context),        /*scope_type*/
                       PointerType::getUnqual(Context), /*funcname*/
                       PointerType::getUnqual(Context), /*filename*/
                       Type::getInt32Ty(Context),       /*line*/
                       Type::getInt32Ty(Context)},      /*col*/
                       false)
  );
  FunctionCallee ExitFunc = M->getOrInsertFunction(
    "cats_trace_instrument_scope_exit",
    FunctionType::get(Type::getVoidTy(Context),
                      {Type::getInt64Ty(Context),       /*call_id*/
                       Type::getInt32Ty(Context),       /*scope_id*/
                       PointerType::getUnqual(Context), /*funcname*/
                       PointerType::getUnqual(Context), /*filename*/
                       Type::getInt32Ty(Context),       /*line*/
                       Type::getInt32Ty(Context)},      /*col*/
                       false)
  );

  // Get debug location information
  const DebugLoc &DL = F.getEntryBlock().begin()->getDebugLoc();
  unsigned Line = 0;
  unsigned Col = 0;
  StringRef Filename = "unknown";

  if (DL) {
    Line = DL.getLine();
    Col = DL.getCol();

    // Try to get the filename from debug info
    if (const DILocation *DIL = DL.get()) {
      Filename = DIL->getFilename();
    }
  }

  Constant *ScopeID = ConstantInt::get(
    Type::getInt32Ty(M->getContext()), g_cats_instrument_scope_id++);

  // Create a global string constant for the filename
  Constant *FilenameStr =
      ConstantDataArray::getString(M->getContext(), Filename);
  Constant *FuncnameStr =
      ConstantDataArray::getString(M->getContext(), F.getName());
  GlobalVariable *FilenameGV = new GlobalVariable(
      *M, FilenameStr->getType(), true, GlobalValue::PrivateLinkage,
      FilenameStr, "filename");
  GlobalVariable *FuncnameGV = new GlobalVariable(
      *M, FuncnameStr->getType(), true, GlobalValue::PrivateLinkage,
      FuncnameStr, "funcname");

  // Create a pointer to the first character of the string
  Constant *Zero =
      ConstantInt::get(Type::getInt32Ty(M->getContext()), 0);
  Constant *Indices[] = {Zero, Zero};
  Constant *FilenamePtr = ConstantExpr::getGetElementPtr(
      FilenameStr->getType(), FilenameGV, Indices, true);
  Constant *FuncnamePtr = ConstantExpr::getGetElementPtr(
      FilenameStr->getType(), FuncnameGV, Indices, true);

  Value *Args[] = {
      ConstantInt::get(Type::getInt64Ty(Context), g_cats_instrument_call_id++),
      ScopeID,
      ConstantInt::get(Type::getInt8Ty(Context), CATS_SCOPE_TYPE_FUNCTION),
      FuncnamePtr,
      FilenamePtr,
      ConstantInt::get(Type::getInt32Ty(M->getContext()), Line),
      ConstantInt::get(Type::getInt32Ty(M->getContext()), Col)};

  // Insert function entry instrumentation at the beginning of the function
  IRBuilder<> Builder(&*F.getEntryBlock().getFirstInsertionPt());
  Builder.CreateCall(EnterFunc, Args);

  // Insert instrumentation before each return instruction
  for (BasicBlock &BB : F) {
    Instruction *Terminator = BB.getTerminator();
    if (ReturnInst *RI = dyn_cast<ReturnInst>(Terminator)) {
      Builder.SetInsertPoint(RI);
      instrumentExit(Builder, ExitFunc, ScopeID, FuncnamePtr,
                     FilenamePtr, RI);
    }
  }

  // Handle unwind paths (exceptions)
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (InvokeInst *II = dyn_cast<InvokeInst>(&I)) {
        // Insert in the unwind destination (landing pad)
        BasicBlock *UnwindDest = II->getUnwindDest();
        Builder.SetInsertPoint(&*UnwindDest->getFirstInsertionPt());
        instrumentExit(Builder, ExitFunc, ScopeID, FuncnamePtr,
                       FilenamePtr, II);
      }
    }
  }

  // Add instrumentation before each call to @llvm.stackrestore
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (CallInst *CI = dyn_cast<CallInst>(&I)) {
        if (CI->getCalledFunction() &&
            CI->getCalledFunction()->getName() == "llvm.stackrestore") {
          Builder.SetInsertPoint(CI);
          instrumentExit(Builder, ExitFunc, ScopeID, FuncnamePtr,
                         FilenamePtr, CI);
        }
      }
    }
  }

  // TODO: This is most likely not correct at the moment. The instrumentation
  // call is added right before the unreachable instruction, but probably
  // should be added before the proceeding instruction.

  // Add instrumentation to any other exit point (like unreachable)
  for (BasicBlock &BB : F) {
    Instruction *Terminator = BB.getTerminator();
    if (isa<UnreachableInst>(Terminator)) {
      outs() << "Terminator: " << *Terminator << "\n";
      Builder.SetInsertPoint(Terminator);
      instrumentExit(Builder, ExitFunc, ScopeID, FuncnamePtr,
                     FilenamePtr, Terminator);
    }/* else
    if (!isa<ReturnInst>(Terminator) && !isa<InvokeInst>(Terminator)) {
      Builder.SetInsertPoint(Terminator);
      instrumentExit(Builder, ExitFunc, ScopeID, FuncnamePtr,
                     FilenamePtr, Terminator);
    }*/
  }

  if (Modified && !g_cats_save_inserted) {
    insertCatsTraceSave(*M);
  }

  if (Modified)
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}
