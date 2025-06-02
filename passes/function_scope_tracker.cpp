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

llvm::AnalysisKey OMPScopeFinder::Key;

OMPScopeFinder::Result OMPScopeFinder::run(
  Module &M, ModuleAnalysisManager &AM
) {
  Result Res;

  // Instrument OpenMP fork calls
  static const std::set<std::string> gomp_fork_names = {
    "GOMP_parallel_start",
    "GOMP_parallel",
  };
  static const std::set<std::string> kmpc_fork_names = {
    "__kmpc_fork_call",
    "__kmpc_fork_teams"
  };

  for (Function &F : M) {
    for (BasicBlock &BB : F) {
      for (auto Inst = BB.begin(); Inst != BB.end(); ++Inst) {
        if (CallInst *CI = dyn_cast<CallInst>(&*Inst)) {
          Function *Callee = CI->getCalledFunction();
          const std::string Name = Callee ? Callee->getName().str() : "";
          Value *TargetFunction = nullptr;
          if (gomp_fork_names.count(Name)) {
            if (CI->arg_size() >= 1) {
              TargetFunction = CI->getArgOperand(0);
            }
            Res.OmpForkCalls.insert(CI);
          } else if (kmpc_fork_names.count(Name)) {
            if (CI->arg_size() >= 3) {
              TargetFunction = CI->getArgOperand(2);
            }
            Res.OmpForkCalls.insert(CI);
          }
          if (TargetFunction != nullptr) {
            // Strip away any bitcasts to get the actual function
            while (BitCastInst *BCI = dyn_cast<BitCastInst>(TargetFunction)) {
              TargetFunction = BCI->getOperand(0);
            }
            if (Function *TargetFunc = dyn_cast<Function>(TargetFunction)) {
              Res.OutlinedFunctions.insert(TargetFunc->getName().str());
            }
          }
        }
      }
    }
  }

  return Res;
}

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
  Module *M = Inst->getModule();
  Value *ExitArgs[] = {
      ConstantInt::get(Type::getInt64Ty(Context), getCurrentCallID(*M, true)),
      ScopeID,
      FuncNamePtr,
      FilenamePtr,
      ConstantInt::get(Type::getInt32Ty(Context), Line),
      ConstantInt::get(Type::getInt32Ty(Context), Col)};
  Builder.CreateCall(ExitFunc, ExitArgs);
}

bool processFunction(Module &M, Function &F, bool parallel) {
  if (F.hasFnAttribute("cats_function_instrumented")) {
    errs() << "Function " << F.getName()
           << " is already instrumented, skipping.\n";
    return false;
  }

  LLVMContext &Context = M.getContext();

  // Get or create the instrument functions
  FunctionCallee EnterFunc = M.getOrInsertFunction(
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
  FunctionCallee ExitFunc = M.getOrInsertFunction(
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
    Type::getInt32Ty(M.getContext()), getCurrentScopeID(M, true)
  );

  // Create a global string constant for the filename
  Constant *FilenameStr = ConstantDataArray::getString(Context, Filename);
  Constant *FuncnameStr = ConstantDataArray::getString(Context, F.getName());
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

  auto *ScopeType = parallel ? ConstantInt::get(
    Type::getInt8Ty(Context), CATS_SCOPE_TYPE_PARALLEL
  ) : ConstantInt::get(
    Type::getInt8Ty(Context), CATS_SCOPE_TYPE_FUNCTION
  );
  Value *Args[] = {
      ConstantInt::get(Type::getInt64Ty(Context), getCurrentCallID(M, true)),
      ScopeID,
      ScopeType,
      FuncnamePtr,
      FilenamePtr,
      ConstantInt::get(Type::getInt32Ty(Context), Line),
      ConstantInt::get(Type::getInt32Ty(Context), Col)};

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

  // Mark the function as instrumented
  errs() << "Instrumenting function: " << F.getName() << "\n";
  F.addFnAttr("cats_function_instrumented");

  return true;
}

PreservedAnalyses FunctionScopeTrackerPass::run(
  Module &M, ModuleAnalysisManager &MAM
) {
  bool Modified = false;
  for (Function &F : M) {
    if (F.isDeclaration()) continue;

    // Skip functions with the "cats_noinstrument" annotation
    if (functionHasAnnotation(F, "cats_noinstrument")) {
      outs() << "Skipping function " << F.getName() << "\n";
      continue;
    }

    auto &ModuleOMPRes = MAM.getResult<OMPScopeFinder>(M);
    bool isParallel = false;
    if (ModuleOMPRes.OutlinedFunctions.count(F.getName().str())) {
      isParallel = true;
    }

    // Process function scopes
    if (processFunction(M, F, isParallel)) {
      // If we modified the function, we assume that nothing is preserved
      Modified = true;
    }
  }

  if (Modified) {
    insertCatsTraceSave(M);
    // Assuming conservatively that nothing is preserved
    return PreservedAnalyses::none();
  }

  return PreservedAnalyses::all();
}
