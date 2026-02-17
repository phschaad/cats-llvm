// Copyright (c) ETH Zurich, the cats-llvm authors, and Lawrence Livermore National Security, LLC. All rights reserved.

#ifndef __CATS_PASSES_HPP__
#define __CATS_PASSES_HPP__


#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

#include <cstdint>
#include <set>

#define CATS_PASSES_VERSION "0.1.0"

#define ALLOCATION_TRACKER_PASS_NAME      "cats-allocation-tracker"
#define LOAD_STORE_TRACKER_PASS_NAME      "cats-load-store-tracker"
#define FUNCTION_SCOPE_TRACKER_PASS_NAME  "cats-function-scope-tracker"
#define LOOP_SCOPE_TRACKER_PASS_NAME      "cats-loop-scope-tracker"
#define PARALLEL_SCOPE_TRACKER_PASS_NAME  "cats-parallel-scope-tracker"


void insertCatsTraceSave(llvm::Module &M);
int getCurrentScopeID(llvm::Module &M, bool increment = true);
uint64_t generateUniqueInt64ID();
int getCurrentCallID(llvm::Module &M, bool increment = true);
bool functionHasAnnotation(llvm::Function &F, llvm::StringRef Annotation);

class AllocationTracker : public llvm::FunctionPass {
public:
  static char ID;
  AllocationTracker() : llvm::FunctionPass(ID) {}

  bool runOnFunction(llvm::Function &F);

private:

  void findVariableNamesFromDbgIntrinsics(
    llvm::Value *AllocValue, std::set<std::string> &Names
  );
  void findVariableNamesFromStores(
    llvm::CallInst *ZnamCall, std::set<std::string> &Names
  );
  void findDebugInfoForAlloca(
    llvm::AllocaInst *AI, std::set<std::string> &Names
  );
  void findVariableNamesFromUses(
    llvm::CallInst *ZnamCall, std::set<std::string> &Names
  );
  void analyzeValueFlow(
    llvm::Value *V, std::set<std::string> &Names,
    std::set<llvm::Value*> &Visited, int Depth
  );
};

struct AllocationTrackerPass : llvm::PassInfoMixin<AllocationTrackerPass> {
  AllocationTrackerPass() {}

  llvm::PreservedAnalyses run(
    llvm::Function &M,
    [[maybe_unused]] llvm::FunctionAnalysisManager &AM
  ) {
    AllocationTracker ATP;

    bool Changed = ATP.runOnFunction(M);
    if (Changed)
      // Assuming conservatively that nothing is preserved
      return llvm::PreservedAnalyses::none();

    return llvm::PreservedAnalyses::all();
  }

  // for optnone
  static bool isRequired() { return true; }
};

class LoadStoreTracker : public llvm::FunctionPass {
public:
  static char ID;
  LoadStoreTracker() : llvm::FunctionPass(ID) {}

  bool runOnFunction(llvm::Function &F);
};

struct LoadStoreTrackerPass : llvm::PassInfoMixin<LoadStoreTrackerPass> {
  LoadStoreTrackerPass() {}

  llvm::PreservedAnalyses run(
    llvm::Function &M,
    [[maybe_unused]] llvm::FunctionAnalysisManager &AM
  ) {
    LoadStoreTracker LSTP;

    bool Changed = LSTP.runOnFunction(M);
    if (Changed)
      // Assuming conservatively that nothing is preserved
      return llvm::PreservedAnalyses::none();

    return llvm::PreservedAnalyses::all();
  }

  // for optnone
  static bool isRequired() { return true; }
};

class FunctionScopeTrackerPass : public llvm::PassInfoMixin<FunctionScopeTrackerPass> {
public:
  FunctionScopeTrackerPass() {}

  llvm::PreservedAnalyses run(
    llvm::Module &M,
    [[maybe_unused]] llvm::ModuleAnalysisManager &AM
  );

  // for optnone
  static bool isRequired() { return true; }
};

class LoopScopeTrackerPass : public llvm::PassInfoMixin<LoopScopeTrackerPass> {
public:
  LoopScopeTrackerPass() {}

  llvm::PreservedAnalyses run(
    llvm::Function &F,
    [[maybe_unused]] llvm::FunctionAnalysisManager &AM
  );

  // for optnone
  static bool isRequired() { return true; }

private:
  void processLoop(
    llvm::Loop *L, llvm::FunctionCallee EntryFunc,
    llvm::FunctionCallee ExitFunc
  );
};

struct OMPScopeFinder : llvm::AnalysisInfoMixin<OMPScopeFinder> {
  OMPScopeFinder() {}

  static llvm::AnalysisKey Key;

  struct Result {
    std::set<llvm::CallInst *> OmpForkCalls;
    std::set<std::string> OutlinedFunctions;
  };

  Result run(
    llvm::Module &M,
    [[maybe_unused]] llvm::ModuleAnalysisManager &AM
  );

  // for optnone
  static bool isRequired() { return true; }
};

class ParallelScopeTrackerPass :
public llvm::PassInfoMixin<ParallelScopeTrackerPass> {
public:
  ParallelScopeTrackerPass() {}

  llvm::PreservedAnalyses run(
    llvm::Module &M,
    [[maybe_unused]] llvm::ModuleAnalysisManager &AM
  );

  // for optnone
  static bool isRequired() { return true; }
};

#endif // __CATS_PASSES_HPP__
