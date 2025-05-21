#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

#include <set>

#include "../runtime/cats_runtime.h"

using namespace llvm;

std::set<std::string> alloc_names = {
    "malloc",
    "realloc",       // TODO: use second argument for size
    "calloc",        // TODO: use first * second arguments for total size
    "aligned_alloc", // TODO: use second argument for size
    "_Znam",         "_Znwm",
    //"posix_memalign", // Needs to be handled separately
};

std::set<std::string> dealloc_names = {
    "free",
    "_ZdlPv",
    "_ZdaPv",
};

// All names (makes querying faster)
std::set<std::string> names = {
    "malloc", "realloc", "calloc", "aligned_alloc", "_Znam",
    "_Znwm",  "free",    "_ZdlPv", "_ZdaPv",
};

static long g_cats_instrument_call_id = 0;

namespace {
class CallTracker : public FunctionPass {
public:
  static char ID;
  CallTracker() : FunctionPass(ID) {}

  bool runOnFunction(Function &F) override {
    bool Modified = false;
    Module *M = F.getParent();

    // Create instrumentation function definitions
    FunctionCallee InstrumentFunc = M->getOrInsertFunction(
        "cats_trace_instrument_alloc",
        FunctionType::get(Type::getVoidTy(M->getContext()),
                          {Type::getInt64Ty(M->getContext()),       /*call_id*/
                           PointerType::getUnqual(M->getContext()), /*name*/
                           PointerType::getUnqual(M->getContext()), /*value*/
                           Type::getInt64Ty(M->getContext()),       /*size*/
                           PointerType::getUnqual(M->getContext()), /*funcname*/
                           PointerType::getUnqual(M->getContext()), /*filename*/
                           Type::getInt32Ty(M->getContext()),       /*line*/
                           Type::getInt32Ty(M->getContext())},      /*col*/
                          false));
    FunctionCallee InstrumentDeallocFunc = M->getOrInsertFunction(
        "cats_trace_instrument_dealloc",
        FunctionType::get(Type::getVoidTy(M->getContext()),
                          {Type::getInt64Ty(M->getContext()),       /*call_id*/
                           PointerType::getUnqual(M->getContext()), /*value*/
                           PointerType::getUnqual(M->getContext()), /*funcname*/
                           PointerType::getUnqual(M->getContext()), /*filename*/
                           Type::getInt32Ty(M->getContext()),       /*line*/
                           Type::getInt32Ty(M->getContext())},      /*col*/
                          false));

    // Iterate through all instructions in the function
    for (auto &BB : F) {
      for (auto Inst = BB.begin(); Inst != BB.end(); ++Inst) {
        // Check if the instruction is a call instruction
        if (CallInst *Call = dyn_cast<CallInst>(&*Inst)) {
          // Check if the callee is an alloc/dealloc function
          Function *Callee = Call->getCalledFunction();
          if (Callee &&
              names.find(std::string{Callee->getName()}) != names.end()) {
            // Create IRBuilder to insert the new call instruction after the
            // call
            ++Inst;

            // Check if called before
            if (CallInst *Call2 = dyn_cast<CallInst>(&*Inst)) {
              Function *Callee2 = Call2->getCalledFunction();
              if (Callee2 &&
                  (Callee2->getName() == "cats_trace_instrument_alloc" ||
                   Callee2->getName() == "cats_trace_instrument_dealloc")) {
                --Inst;
                continue;
              }
            }
            // END of duplicate check

            IRBuilder<> Builder(&*Inst);
            --Inst; // Move back to the original call instruction

            // Get debug location information
            const DebugLoc &DL = Call->getDebugLoc();
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

            // Try to get variable name (looping over Users() did not work for
            // some reason)
            // TODO: Use Users() somehow, or directly get debuginfo?
            std::string varname{Call->getName()};
            for (auto &nextbb : F) {
              for (auto NextInst = nextbb.begin(); NextInst != nextbb.end();
                   ++NextInst) {
                if (auto *DVI =
                        llvm::dyn_cast<llvm::DbgValueInst>(&*NextInst)) {
                  llvm::Value *dbgVal = DVI->getValue();
                  if (dbgVal && dbgVal == Call) {
                    if (llvm::DILocalVariable *var = DVI->getVariable()) {
                      varname = var->getName();
                    }
                  }
                } else if (auto *DDI = llvm::dyn_cast<llvm::DbgDeclareInst>(
                               &*NextInst)) {
                  llvm::Value *dbgVal = DDI->getAddress();
                  if (dbgVal && dbgVal->stripPointerCasts() == Call) {
                    if (llvm::DILocalVariable *var = DDI->getVariable()) {
                      varname = var->getName();
                    }
                  }
                } /* else if (auto *CI =
                 llvm::dyn_cast<llvm::CallInst>(&*NextInst)) { if
                 (CI->getCalledFunction()->getName() == "llvm.dbg.value") {
                     llvm::errs() << *CI << "\n";

                   }
                 }*/
              }
            }

            // Crate a call ID constant
            Constant *CallID =
                ConstantInt::get(Type::getInt64Ty(M->getContext()),
                                 g_cats_instrument_call_id++);

            // Create a global string constant for the filename
            Constant *FilenameStr =
                ConstantDataArray::getString(M->getContext(), Filename);
            Constant *FuncnameStr = ConstantDataArray::getString(
                M->getContext(), Callee->getName());
            Constant *ValnameStr =
                ConstantDataArray::getString(M->getContext(), varname);
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

            // Create a call to cats_trace_instrument_* with filename, line, and
            // column numbers
            if (alloc_names.find(std::string{Callee->getName()}) !=
                alloc_names.end()) {
              GlobalVariable *ValnameGV = new GlobalVariable(
                  *M, ValnameStr->getType(), true, GlobalValue::PrivateLinkage,
                  ValnameStr, "valname");
              Constant *ValnamePtr = ConstantExpr::getGetElementPtr(
                  FilenameStr->getType(), ValnameGV, Indices, true);
              Value *Args[] = {
                  CallID,
                  ValnamePtr,
                  Call,
                  Call->getArgOperand(0),
                  FuncnamePtr,
                  FilenamePtr,
                  ConstantInt::get(Type::getInt32Ty(M->getContext()), Line),
                  ConstantInt::get(Type::getInt32Ty(M->getContext()), Col)};
              Builder.CreateCall(InstrumentFunc, Args);
            } else {
              Value *Args[] = {
                  CallID,
                  Call->getArgOperand(0), FuncnamePtr, FilenamePtr,
                  ConstantInt::get(Type::getInt32Ty(M->getContext()), Line),
                  ConstantInt::get(Type::getInt32Ty(M->getContext()), Col)};
              Builder.CreateCall(InstrumentDeallocFunc, Args);
            }

            Modified = true;

            // Since we modified the instruction stream, we need to adjust the
            // iterator to avoid skipping the next instruction
            ++Inst;
            if (Inst == BB.end())
              break;
          }
        }
      }
    }

    return Modified;
  }
};
} // namespace

struct CallTrackerPass : PassInfoMixin<CallTrackerPass> {
  CallTrackerPass() {}

  PreservedAnalyses run(Function &M,
                        [[maybe_unused]] FunctionAnalysisManager &AM) {
    CallTracker CTP;

    bool Changed = CTP.runOnFunction(M);
    if (Changed)
      // Assuming conservatively that nothing is preserved
      return PreservedAnalyses::none();

    return PreservedAnalyses::all();
  }

  // for optnone
  static bool isRequired() { return true; }
};

namespace {
class LoadStoreTracker : public FunctionPass {
public:
  static char ID;
  LoadStoreTracker() : FunctionPass(ID) {}

  bool runOnFunction(Function &F) override {
    bool Modified = false;
    Module *M = F.getParent();

    // Create instrumentation function definitions
    FunctionCallee InstrumentFunc = M->getOrInsertFunction(
        "cats_trace_instrument_access",
        FunctionType::get(Type::getVoidTy(M->getContext()),
                          {Type::getInt64Ty(M->getContext()),       /*call_id*/
                           PointerType::getUnqual(M->getContext()), /*value*/
                           Type::getInt1Ty(M->getContext()),        /*is_write*/
                           PointerType::getUnqual(M->getContext()), /*funcname*/
                           PointerType::getUnqual(M->getContext()), /*filename*/
                           Type::getInt32Ty(M->getContext()),       /*line*/
                           Type::getInt32Ty(M->getContext())},      /*col*/
                          false));

    // Iterate through all instructions in the function
    for (auto &BB : F) {
      for (auto Inst = BB.begin(); Inst != BB.end(); ++Inst) {
        // Check if the instruction is a load/store
        Value *val = nullptr;
        bool is_write = false;
        if (LoadInst *linst = dyn_cast<LoadInst>(&*Inst)) {
          val = linst->getPointerOperand();
        } else if (StoreInst *sinst = dyn_cast<StoreInst>(&*Inst)) {
          is_write = true;
          val = sinst->getPointerOperand();
        } else {
          continue;
        }
        if (Inst->getType()->isPointerTy()) {
          llvm::errs() << "Skipping (ptr) " << *Inst << "\n";
          continue;
        }
        if (AllocaInst *ainst = dyn_cast<AllocaInst>(val)) {
          llvm::errs() << "Skipping (local alloca) " << *Inst << "\n";
          continue;
        }

        // Create IRBuilder to insert the new call instruction after the
        // load/store
        ++Inst;

        // Check if called before
        if (CallInst *Call2 = dyn_cast<CallInst>(&*Inst)) {
          Function *Callee2 = Call2->getCalledFunction();
          if (Callee2 && Callee2->getName() == "cats_trace_instrument_access") {
            --Inst;
            continue;
          }
        }
        // END of duplicate check

        IRBuilder<> Builder(&*Inst);
        --Inst; // Move back to the original call instruction

        // Get debug location information
        DebugLoc DL = Inst->getDebugLoc();
        unsigned Line = 0;
        unsigned Col = 0;
        StringRef Filename = "unknown";

        if (!DL) {
          // No debuginfo, could it be a GEP we can trace back?
          if (llvm::GetElementPtrInst *gep =
                  llvm::dyn_cast<llvm::GetElementPtrInst>(val)) {
            DL = gep->getDebugLoc();
            if (!DL) {
              Value *gepptr = gep->getPointerOperand();
              if (Instruction *gepptrinst = dyn_cast<Instruction>(gepptr)) {
                DL = gepptrinst->getDebugLoc();
              }
            }
          }
        }

        if (DL) {
          Line = DL.getLine();
          Col = DL.getCol();

          // Try to get the filename from debug info
          if (const DILocation *DIL = DL.get()) {
            Filename = DIL->getFilename();
          }
        }

        // Create a call ID constant
        Constant *CallID =
            ConstantInt::get(Type::getInt64Ty(M->getContext()),
                             g_cats_instrument_call_id++);

        // Create a global string constant for the filename
        Constant *FilenameStr =
            ConstantDataArray::getString(M->getContext(), Filename);
        Constant *FuncnameStr = ConstantDataArray::getString(
            M->getContext(), F.getName());
        GlobalVariable *FilenameGV = new GlobalVariable(
            *M, FilenameStr->getType(), true, GlobalValue::PrivateLinkage,
            FilenameStr, "filename");
        GlobalVariable *FuncnameGV = new GlobalVariable(
            *M, FuncnameStr->getType(), true, GlobalValue::PrivateLinkage,
            FuncnameStr, "funcname");

        // Create a pointer to the first character of the string
        Constant *Zero = ConstantInt::get(Type::getInt32Ty(M->getContext()), 0);
        Constant *Indices[] = {Zero, Zero};
        Constant *FilenamePtr = ConstantExpr::getGetElementPtr(
            FilenameStr->getType(), FilenameGV, Indices, true);
        Constant *FuncnamePtr = ConstantExpr::getGetElementPtr(
            FilenameStr->getType(), FuncnameGV, Indices, true);

        // Create a call to cats_trace_instrument_access with filename, line,
        // and column numbers
        Value *Args[] = {
            CallID,
            val, ConstantInt::get(Type::getInt1Ty(M->getContext()), is_write),
            FuncnamePtr, FilenamePtr,
            ConstantInt::get(Type::getInt32Ty(M->getContext()), Line),
            ConstantInt::get(Type::getInt32Ty(M->getContext()), Col)};
        Builder.CreateCall(InstrumentFunc, Args);

        Modified = true;

        // Since we modified the instruction stream, we need to adjust the
        // iterator to avoid skipping the next instruction
        ++Inst;
        if (Inst == BB.end())
          break;
      }
    }

    return Modified;
  }
};
} // namespace

struct LoadStoreTrackerPass : PassInfoMixin<LoadStoreTrackerPass> {
  LoadStoreTrackerPass() {}

  PreservedAnalyses run(Function &M,
                        [[maybe_unused]] FunctionAnalysisManager &AM) {
    LoadStoreTracker LSTP;

    bool Changed = LSTP.runOnFunction(M);
    if (Changed)
      // Assuming conservatively that nothing is preserved
      return PreservedAnalyses::none();

    return PreservedAnalyses::all();
  }

  // for optnone
  static bool isRequired() { return true; }
};


llvm::PassPluginLibraryInfo getCallTrackerPassPluginInfo() {
  const auto Callback = [](PassBuilder &PB) {
    // Only runs on `clang -O1..3` (not -O0!)
    PB.registerPeepholeEPCallback([&](FunctionPassManager &FPM, auto) {
      FPM.addPass(CallTrackerPass());
      return true;
    });

    // Only runs with `opt -passes call-tracker`
    PB.registerPipelineParsingCallback(
        [](StringRef Name, FunctionPassManager &FPM,
           ArrayRef<PassBuilder::PipelineElement>) {
          if (Name == "call-tracker") {
            FPM.addPass(CallTrackerPass());
            return true;
          } else if (Name == "access-tracker") {
            FPM.addPass(LoadStoreTrackerPass());
            return true;
          }

          return false;
        });
  };

  return {LLVM_PLUGIN_API_VERSION, "CallTracker", LLVM_VERSION_STRING,
          Callback};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getCallTrackerPassPluginInfo();
}

char CallTracker::ID = 0;
char LoadStoreTracker::ID = 1;
