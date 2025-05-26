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
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <set>

#include "../runtime/cats_runtime.h"

// Debug records are available since LLVM 17
#if LLVM_VERSION_MAJOR >= 17
#include "llvm/IR/DebugProgramInstruction.h"
#define HAVE_DEBUG_RECORDS 1
#else
#define HAVE_DEBUG_RECORDS 0
#endif

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
static bool g_cats_save_inserted = false;

void insertCatsTraceSave(Module &M) {
  if (g_cats_save_inserted)
    return;

  // Insert declaration for cats_trace_save
  outs() << "Inserting cats_trace_save\n";
  FunctionCallee SaveFunc = M.getOrInsertFunction(
      "cats_trace_save",
      FunctionType::get(Type::getVoidTy(M.getContext()),
                        {PointerType::getUnqual(M.getContext())}, /*filename*/
                        false));
  appendToGlobalDtors(M, cast<Function>(SaveFunc.getCallee()), 0);
  g_cats_save_inserted = true;
}

bool functionHasAnnotation(Function &F, StringRef Annotation) {
  // Look for llvm.global.annotations which stores __attribute__((annotate(...)))
  Module *M = F.getParent();
  GlobalVariable *Annotations = M->getGlobalVariable("llvm.global.annotations");

  if (!Annotations) return false;

  ConstantArray *CA = dyn_cast<ConstantArray>(Annotations->getInitializer());
  if (!CA) return false;

  for (unsigned i = 0; i < CA->getNumOperands(); ++i) {
    ConstantStruct *CS = dyn_cast<ConstantStruct>(CA->getOperand(i));
    if (!CS) continue;

    // First element should be the function
    if (CS->getOperand(0)->stripPointerCasts() == &F) {
      // Second element is the annotation string
      if (GlobalVariable *AnnotationGV =
          dyn_cast<GlobalVariable>(CS->getOperand(1)->stripPointerCasts())) {
        if (ConstantDataArray *CDA =
            dyn_cast<ConstantDataArray>(AnnotationGV->getInitializer())) {
          if (CDA->getAsCString() == Annotation) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

namespace {
class CallTracker : public FunctionPass {
public:
  static char ID;
  CallTracker() : FunctionPass(ID) {}

  bool runOnFunction(Function &F) override {
    if (functionHasAnnotation(F, "cats_noinstrument")) {
      llvm::errs() << "Skipping function " << F.getName() << "\n";
      return false;
    }

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

            std::set<std::string> AllocNames;
            this->findVariableNamesFromDbgIntrinsics(Call, AllocNames);
            this->findVariableNamesFromStores(Call, AllocNames);
            this->findVariableNamesFromUses(Call, AllocNames);

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

            // Get the variable name for the allocation
            std::string varname;
            if (AllocNames.empty()) {
              // If no names found, use the function name as a fallback
              varname = Callee->getName().str();
            } else {
              // Use the first name found
              varname = *AllocNames.begin();
              if (AllocNames.size() > 1) {
                errs() << "Warning: Multiple variable names found for "
                      << Callee->getName() << ": ";
                for (const auto &Name : AllocNames) {
                  errs() << Name << " ";
                  errs() << "\n";
                }
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

    if (Modified && !g_cats_save_inserted) {
      insertCatsTraceSave(*M);
    }

    return Modified;
  }

private:

  void findVariableNamesFromDbgIntrinsics(
    Value *AllocValue, std::set<std::string> &Names
  ) {
    // Look through the function for debug intrinsics that reference this value
    Function *F = nullptr;
    if (Instruction *I = dyn_cast<Instruction>(AllocValue)) {
      F = I->getFunction();
    }
    if (!F) return;

    for (BasicBlock &BB : *F) {
      for (Instruction &I : BB) {
#if HAVE_DEBUG_RECORDS
        // Check debug records attached to the instruction (LLVM 17+)
        for (DbgRecord &DR : I.getDbgRecordRange()) {
          if (DbgVariableRecord *DVR = dyn_cast<DbgVariableRecord>(&DR)) {
            // Check if this debug record references our allocation
            for (auto Op : DVR->location_ops()) {
              if (Op == AllocValue) {
                if (DILocalVariable *Var = DVR->getVariable()) {
                  Names.insert(Var->getName().str());
                }
                break;
              }
            }
          }
        }
#endif

        // Check legacy debug intrinsics (all LLVM versions)
        if (DbgValueInst *DVI = dyn_cast<DbgValueInst>(&I)) {
          if (DVI->getValue() == AllocValue) {
            if (DILocalVariable *Var = DVI->getVariable()) {
              Names.insert(Var->getName().str());
            }
          }
        } else if (DbgDeclareInst *DDI = dyn_cast<DbgDeclareInst>(&I)) {
          if (DDI->getAddress() == AllocValue) {
            if (DILocalVariable *Var = DDI->getVariable()) {
              Names.insert(Var->getName().str());
            }
          }
        }
      }
    }
  }

  void findVariableNamesFromStores(
    CallInst *ZnamCall, std::set<std::string> &Names
  ) {
    // Look for stores of the allocation result to local variables
    for (User *U : ZnamCall->users()) {
      if (StoreInst *SI = dyn_cast<StoreInst>(U)) {
        Value *Ptr = SI->getPointerOperand();

        // Look for debug info associated with the stored-to location
        if (AllocaInst *AI = dyn_cast<AllocaInst>(Ptr)) {
          findDebugInfoForAlloca(AI, Names);
        }
              
#if HAVE_DEBUG_RECORDS
        // Check for debug records attached to the store instruction (LLVM 17+)
        for (DbgRecord &DR : SI->getDbgRecordRange()) {
          if (DbgVariableRecord *DVR = dyn_cast<DbgVariableRecord>(&DR)) {
            // Check if this debug record references the stored value or pointer
            // Alternatively, check if this is a declare-type recordk
            for (auto Op : DVR->location_ops()) {
              if (Op == ZnamCall || Op == Ptr) {
                if (DILocalVariable *Var = DVR->getVariable()) {
                  Names.insert(Var->getName().str());
                }
                break;
              }
            }
          }
        }
#endif
        // Check for subsequent debug info (both legacy and new formats)
        BasicBlock::iterator It(SI);
        ++It;
        for (int i = 0; i < 10 && It != SI->getParent()->end(); ++It, ++i) {
#if HAVE_DEBUG_RECORDS
          // Check debug records on subsequent instructions (LLVM 17+)
          for (DbgRecord &DR : It->getDbgRecordRange()) {
            if (DbgVariableRecord *DVR = dyn_cast<DbgVariableRecord>(&DR)) {
              for (auto Op : DVR->location_ops()) {
                if (Op == ZnamCall || Op == Ptr) {
                  if (DILocalVariable *Var = DVR->getVariable()) {
                    Names.insert(Var->getName().str());
                  }
                  break;
                }
              }
            }
          }
#endif
          // Legacy debug intrinsics (all LLVM versions)
          if (DbgDeclareInst *DDI = dyn_cast<DbgDeclareInst>(&*It)) {
            if (DDI->getAddress() == Ptr) {
              if (DILocalVariable *Var = DDI->getVariable()) {
                Names.insert(Var->getName().str());
              }
            }
          } else if (DbgValueInst *DVI = dyn_cast<DbgValueInst>(&*It)) {
            if (DVI->getValue() == ZnamCall || DVI->getValue() == Ptr) {
              if (DILocalVariable *Var = DVI->getVariable()) {
                Names.insert(Var->getName().str());
              }
            }
          }
        }
      }
    }
  }

  void findDebugInfoForAlloca(AllocaInst *AI, std::set<std::string> &Names) {
#if HAVE_DEBUG_RECORDS
    // Look for debug records attached to the alloca (LLVM 17+)
    for (DbgRecord &DR : AI->getDbgRecordRange()) {
      if (DbgVariableRecord *DVR = dyn_cast<DbgVariableRecord>(&DR)) {
        // Check if this is a declare-type record for this alloca
        if (DVR->getType() == DbgVariableRecord::LocationType::Declare) {
          for (auto Op : DVR->location_ops()) {
            if (Op == AI) {
              if (DILocalVariable *Var = DVR->getVariable()) {
                Names.insert(Var->getName().str());
              }
              break;
            }
          }
        }
      }
    }
#endif

    // Look for debug declare intrinsics for this alloca (all LLVM versions)
    for (User *U : AI->users()) {
      if (DbgDeclareInst *DDI = dyn_cast<DbgDeclareInst>(U)) {
        if (DILocalVariable *Var = DDI->getVariable()) {
          Names.insert(Var->getName().str());
        }
      }
    }
  }

  void findVariableNamesFromUses(
    CallInst *ZnamCall, std::set<std::string> &Names
  ) {
    // Analyze the dataflow to find variable assignments
    std::set<Value*> Visited;
    analyzeValueFlow(ZnamCall, Names, Visited, 0);
  }

  void analyzeValueFlow(
    Value *V, std::set<std::string> &Names, std::set<Value*> &Visited, int Depth
  ) {
    if (Depth > 5 || Visited.count(V)) return;
    Visited.insert(V);

    for (User *U : V->users()) {
      if (StoreInst *SI = dyn_cast<StoreInst>(U)) {
        Value *Ptr = SI->getPointerOperand();
        if (AllocaInst *AI = dyn_cast<AllocaInst>(Ptr)) {
          findDebugInfoForAlloca(AI, Names);
        }
      } else if (BitCastInst *BCI = dyn_cast<BitCastInst>(U)) {
        analyzeValueFlow(BCI, Names, Visited, Depth + 1);
      } else if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(U)) {
        analyzeValueFlow(GEP, Names, Visited, Depth + 1);
      }
    }
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
    if (functionHasAnnotation(F, "cats_noinstrument")) {
      llvm::errs() << "Skipping function " << F.getName() << "\n";
      return false;
    }

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

    if (Modified && !g_cats_save_inserted) {
      insertCatsTraceSave(*M);
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
