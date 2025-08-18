// Copyright (c) ETH Zurich and the cats-llvm authors. All rights reserved.

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

using namespace llvm;

bool LoadStoreTracker::runOnFunction(Function &F) {
  if (functionHasAnnotation(F, "cats_noinstrument")) {
    errs() << "Skipping function " << F.getName() << "\n";
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
      /*
      if (Inst->getType()->isPointerTy()) {
        llvm::errs() << "Skipping (ptr) " << *Inst << "\n";
        continue;
      }
      */
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
      Constant *CallID = ConstantInt::get(
        Type::getInt64Ty(M->getContext()), generateUniqueInt64ID(), false
      );

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
        FilenameStr->getType(), FilenameGV, Indices, true
      );
      Constant *FuncnamePtr = ConstantExpr::getGetElementPtr(
        FilenameStr->getType(), FuncnameGV, Indices, true
      );

      // Create a call to cats_trace_instrument_access with filename, line,
      // and column numbers
      Value *Args[] = {
        CallID,
        val, ConstantInt::get(Type::getInt1Ty(M->getContext()), is_write),
        FuncnamePtr, FilenamePtr,
        ConstantInt::get(Type::getInt32Ty(M->getContext()), Line),
        ConstantInt::get(Type::getInt32Ty(M->getContext()), Col)
      };
      Builder.CreateCall(InstrumentFunc, Args);

      Modified = true;

      // Since we modified the instruction stream, we need to adjust the
      // iterator to avoid skipping the next instruction
      ++Inst;
      if (Inst == BB.end())
        break;
    }
  }

  if (Modified) {
    insertCatsTraceSave(*M);
  }

  return Modified;
}

char LoadStoreTracker::ID = 1;
