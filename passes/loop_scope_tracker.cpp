#include "cats_passes.hpp"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

#include "../runtime/cats_runtime.h"

using namespace llvm;

PreservedAnalyses LoopScopeTrackerPass::run(
  Function &F, FunctionAnalysisManager &AM
) {
  // Skip declarations
  if (F.empty())
    return PreservedAnalyses::all();

  if (functionHasAnnotation(F, "cats_noinstrument")) {
    outs() << "Skipping function " << F.getName() << "\n";
    return PreservedAnalyses::all();
  }

  bool Modified = false;
  
  // Get the loop analysis information
  LoopInfo &LI = AM.getResult<LoopAnalysis>(F);
  Module *M = F.getParent();
  LLVMContext &Context = M->getContext();

  // Get or create the instrument functions
  FunctionCallee EnterFunc = M->getOrInsertFunction(
    "cats_trace_instrument_scope_entry",
    FunctionType::get(Type::getVoidTy(Context),
                      {Type::getInt64Ty(Context),       /*call_id*/
                       Type::getInt64Ty(Context),       /*scope_id*/
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
                       Type::getInt64Ty(Context),       /*scope_id*/
                       Type::getInt8Ty(Context),        /*scope_type*/
                       PointerType::getUnqual(Context), /*funcname*/
                       PointerType::getUnqual(Context), /*filename*/
                       Type::getInt32Ty(Context),       /*line*/
                       Type::getInt32Ty(Context)},      /*col*/
                       false)
  );

  // Process all loops
  for (Loop *L : LI) {
    processLoop(L, EnterFunc, ExitFunc);
    Modified = true;
  }

  if (Modified) {
    // If we modified the function, we assume that nothing is preserved
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}

void LoopScopeTrackerPass::processLoop(
  Loop *L, FunctionCallee EntryFunc, FunctionCallee ExitFunc
) {
  // Get the preheader and exit blocks of the loop
  BasicBlock *Preheader = L->getLoopPreheader();
  SmallVector<BasicBlock *, 8> ExitBlocks;
  L->getExitBlocks(ExitBlocks);
  
  if (!Preheader) {
    // If there's no preheader, we can't safely insert the instrumentation
    errs() << "Warning: Loop without preheader found. Skipping.\n";
    return;
  }

  // Get debug location information
  const DebugLoc &DL = Preheader->begin()->getDebugLoc();
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

  LLVMContext &Context = Preheader->getContext();
  Function *F = Preheader->getParent();
  Module *M = F->getParent();

  Constant *ScopeID = ConstantInt::get(
    Type::getInt64Ty(Context), generateUniqueInt64ID(), false
  );

  Constant *ScopeType = ConstantInt::get(
    Type::getInt8Ty(Context), CATS_SCOPE_TYPE_LOOP
  );

  // Create a global string constant for the filename
  Constant *FilenameStr =
      ConstantDataArray::getString(Context, Filename);
  Constant *FuncnameStr =
      ConstantDataArray::getString(Context, F->getName());
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
      ConstantInt::get(
        Type::getInt64Ty(Context), generateUniqueInt64ID(), false
      ),
      ScopeID,
      ScopeType,
      FuncnamePtr,
      FilenamePtr,
      ConstantInt::get(Type::getInt32Ty(Context), Line),
      ConstantInt::get(Type::getInt32Ty(Context), Col)};

  // Insert the entry instrumentation at the end of the preheader
  IRBuilder<> Builder(Preheader->getTerminator());
  Builder.CreateCall(EntryFunc, Args);
  
  // Insert the exit instrumentation at the beginning of each exit block
  for (BasicBlock *ExitBlock : ExitBlocks) {
    // Get debug location information
    const DebugLoc &DL = ExitBlock->begin()->getDebugLoc();
    unsigned Line = 0;
    unsigned Col = 0;

    if (DL) {
      Line = DL.getLine();
      Col = DL.getCol();
    }
    Value *ExitArgs[] = {
        ConstantInt::get(
          Type::getInt64Ty(Context), generateUniqueInt64ID(), false
        ),
        ScopeID,
        ScopeType,
        FuncnamePtr,
        FilenamePtr,
        ConstantInt::get(Type::getInt32Ty(Context), Line),
        ConstantInt::get(Type::getInt32Ty(Context), Col)};

    Builder.SetInsertPoint(&*ExitBlock->getFirstInsertionPt());
    Builder.CreateCall(ExitFunc, ExitArgs);
  }
  
  // Process nested loops
  for (Loop *SubL : L->getSubLoops()) {
    processLoop(SubL, EntryFunc, ExitFunc);
  }
}
