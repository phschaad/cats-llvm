#include "cats_passes.hpp"

#include "llvm/Pass.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

using namespace llvm;

PassPluginLibraryInfo getCallTrackerPassPluginInfo() {
  const auto Callback = [](PassBuilder &PB) {
    // Only runs on `clang -O1..3` (not -O0!)
    PB.registerPeepholeEPCallback([&](FunctionPassManager &FPM, auto) {
      FPM.addPass(AllocationTrackerPass());
      return true;
    });

    // Only runs with the corresponding `opt -passes` arguments
    PB.registerPipelineParsingCallback(
        [](StringRef Name, FunctionPassManager &FPM,
           ArrayRef<PassBuilder::PipelineElement>) {
          if (Name == ALLOCATION_TRACKER_PASS_NAME) {
            FPM.addPass(AllocationTrackerPass());
            return true;
          } else if (Name == LOAD_STORE_TRACKER_PASS_NAME) {
            FPM.addPass(LoadStoreTrackerPass());
            return true;
          } else if (Name == FUNCTION_SCOPE_TRACKER_PASS_NAME) {
            FPM.addPass(FunctionScopeTrackerPass());
            return true;
          } else if (Name == LOOP_SCOPE_TRACKER_PASS_NAME) {
            FPM.addPass(LoopScopeTrackerPass());
            return true;
          }

          return false;
        });
  };

  return {
    LLVM_PLUGIN_API_VERSION,
    "CATSPasses",
    CATS_PASSES_VERSION,
    Callback,
  };
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getCallTrackerPassPluginInfo();
}