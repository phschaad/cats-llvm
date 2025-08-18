// Copyright (c) ETH Zurich and the cats-llvm authors. All rights reserved.

#include "cats_passes.hpp"

#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include "../runtime/cats_runtime.h"


using namespace llvm;

PreservedAnalyses ParallelScopeTrackerPass::run(
  Module &M, ModuleAnalysisManager &MAM
) {
  bool Modified = false;
  auto &ModuleOMPRes = MAM.getResult<OMPScopeFinder>(M);

  LLVMContext &Context = M.getContext();
  FunctionCallee EnterFunc = M.getOrInsertFunction(
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
  FunctionCallee ExitFunc = M.getOrInsertFunction(
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

  for (Function &F : M) {
    if (F.isDeclaration()) continue;

    // Skip functions with the "cats_noinstrument" annotation
    if (functionHasAnnotation(F, "cats_noinstrument")) {
      outs() << "Skipping function " << F.getName() << "\n";
      continue;
    }

    for (BasicBlock &BB : F) {
      for (auto Inst = BB.begin(); Inst != BB.end(); ++Inst) {
        if (CallInst *CI = dyn_cast<CallInst>(&*Inst)) {
          if (ModuleOMPRes.OmpForkCalls.count(CI)) {
            // Instrument the OpenMP fork calls as parallel scopes.

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
              Type::getInt64Ty(M.getContext()), generateUniqueInt64ID(), false
            );

            Constant *ScopeType = ConstantInt::get(
              Type::getInt8Ty(Context), CATS_SCOPE_TYPE_PARALLEL
            );

            // Create a global string constant for the filename
            Constant *FilenameStr = ConstantDataArray::getString(
              Context, Filename
            );
            Constant *FuncnameStr = ConstantDataArray::getString(
              Context, F.getName()
            );
            GlobalVariable *FilenameGV = new GlobalVariable(
                M, FilenameStr->getType(), true, GlobalValue::PrivateLinkage,
                FilenameStr, "filename");
            GlobalVariable *FuncnameGV = new GlobalVariable(
                M, FuncnameStr->getType(), true, GlobalValue::PrivateLinkage,
                FuncnameStr, "funcname");

            // Create a pointer to the first character of the string
            Constant *Zero = ConstantInt::get(Type::getInt32Ty(Context), 0);
            Constant *Indices[] = {Zero, Zero};
            Constant *FilenamePtr = ConstantExpr::getGetElementPtr(
                FilenameStr->getType(), FilenameGV, Indices, true);
            Constant *FuncnamePtr = ConstantExpr::getGetElementPtr(
                FilenameStr->getType(), FuncnameGV, Indices, true);

            Value *EntryArgs[] = {
                ConstantInt::get(
                  Type::getInt64Ty(Context), generateUniqueInt64ID(), false
                ),
                ScopeID,
                ScopeType,
                FuncnamePtr,
                FilenamePtr,
                ConstantInt::get(Type::getInt32Ty(Context), Line),
                ConstantInt::get(Type::getInt32Ty(Context), Col)};
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

            IRBuilder<> Builder(CI);
            Builder.CreateCall(EnterFunc, EntryArgs);
            Builder.SetInsertPoint(++Inst);
            Builder.CreateCall(ExitFunc, ExitArgs);
            Inst--;
            Modified = true;
          }
        }
      }
    }
  }

  if (Modified) {
    insertCatsTraceSave(M);
    // Assuming conservatively that nothing is preserved
    return PreservedAnalyses::none();
  }

  return PreservedAnalyses::all();
}
