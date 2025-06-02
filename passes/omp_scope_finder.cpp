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


using namespace llvm;

AnalysisKey OMPScopeFinder::Key;

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
