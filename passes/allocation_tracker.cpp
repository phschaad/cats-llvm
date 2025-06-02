#include "cats_passes.hpp"

#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Passes/PassBuilder.h"

#include <set>

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

bool AllocationTracker::runOnFunction(Function &F) {
  if (functionHasAnnotation(F, "cats_noinstrument")) {
    errs() << "Skipping function " << F.getName() << "\n";
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
                               getCurrentCallID(*M, true));

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

  if (Modified) {
    insertCatsTraceSave(*M);
  }

  return Modified;
}

void AllocationTracker::findVariableNamesFromDbgIntrinsics(
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

void AllocationTracker::findVariableNamesFromStores(
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

void AllocationTracker::findDebugInfoForAlloca(
  AllocaInst *AI, std::set<std::string> &Names
) {
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

void AllocationTracker::findVariableNamesFromUses(
  CallInst *ZnamCall, std::set<std::string> &Names
) {
  // Analyze the dataflow to find variable assignments
  std::set<Value*> Visited;
  analyzeValueFlow(ZnamCall, Names, Visited, 0);
}

void AllocationTracker::analyzeValueFlow(
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

char AllocationTracker::ID = 0;
