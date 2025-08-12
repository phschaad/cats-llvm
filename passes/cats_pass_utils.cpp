// Copyright (c) ETH Zurich and the cats-llvm authors. All rights reserved.

#include "cats_passes.hpp"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <random>
#include <sstream>
#include <iomanip>
#include <functional>

using namespace llvm;

void insertCatsTraceSave(Module &M) {
  // Only insert if not already done
  if (NamedMDNode *NMD = M.getNamedMetadata("cats.trace.save_inserted")) {
    for (const MDNode *MD : NMD->operands()) {
      if (auto *MDV = dyn_cast<ConstantAsMetadata>(MD->getOperand(0))) {
        if (auto *CI = dyn_cast<ConstantInt>(MDV->getValue())) {
          if (CI->getZExtValue() == 1) {
            return;
          }
        }
      }
    }
  }

  // Insert declaration for cats_trace_save
  outs() << "Inserting cats_trace_save\n";
  FunctionCallee SaveFunc = M.getOrInsertFunction(
      "cats_trace_save",
      FunctionType::get(Type::getVoidTy(M.getContext()),
                        {PointerType::getUnqual(M.getContext())}, /*filename*/
                        false));
  appendToGlobalDtors(M, cast<Function>(SaveFunc.getCallee()), 0);

  // Create a metadata node to indicate that the save has been inserted
  LLVMContext &Context = M.getContext();
  Constant *SavedConst = ConstantInt::get(Type::getInt1Ty(Context), true);
  MDNode *MetaNode = MDNode::get(Context, ConstantAsMetadata::get(SavedConst));
  NamedMDNode *NMD = M.getOrInsertNamedMetadata("cats.trace.save_inserted");
  NMD->addOperand(MetaNode);
}

int getCurrentScopeID(Module &M, bool increment) {
  if (NamedMDNode *NMD = M.getNamedMetadata("cats.trace.current_scope_id")) {
    for (const MDNode *MD : NMD->operands()) {
      if (auto *MDV = dyn_cast<ConstantAsMetadata>(MD->getOperand(0))) {
        if (auto *CI = dyn_cast<ConstantInt>(MDV->getValue())) {
          int64_t ID = CI->getZExtValue();
          if (increment) {
            // Increment the scope ID for the next use
            Constant *NewID = ConstantInt::get(
              Type::getInt64Ty(M.getContext()),
              ID + 1
            );
            NMD->clearOperands();
            NMD->addOperand(
              MDNode::get(M.getContext(), ConstantAsMetadata::get(NewID))
            );
          }
          return ID;
        }
      }
    }
  }

  // If no current scope ID is found, return 0 and initialize it to 1 (if
  // increment is true)
  int initVal = increment ? 1 : 0;
  Constant *InitialID = ConstantInt::get(
    Type::getInt64Ty(M.getContext()), initVal
  );
  MDNode *InitialNode = MDNode::get(
    M.getContext(), ConstantAsMetadata::get(InitialID)
  );
  NamedMDNode *NMD = M.getOrInsertNamedMetadata("cats.trace.current_scope_id");
  NMD->addOperand(InitialNode);
  return 0;
}

std::string generateUUID() {
  // Generate a UUID string
  static std::random_device rd;
  static std::mt19937 gen(rd());
  static std::uniform_int_distribution<> dis(0, 15);
  static std::uniform_int_distribution<> dis2(8, 11);

  std::stringstream ss;
  int i;
  ss << std::hex;
  for (i = 0; i < 8; i++) {
    ss << dis(gen);
  }
  ss << "-";
  for (i = 0; i < 4; i++) {
    ss << dis(gen);
  }
  ss << "-4";
  for (i = 0; i < 3; i++) {
    ss << dis(gen);
  }
  ss << "-";
  ss << dis2(gen);
  for (i = 0; i < 3; i++) {
    ss << dis(gen);
  }
  ss << "-";
  for (i = 0; i < 12; i++) {
    ss << dis(gen);
  }
  return ss.str();
}

uint64_t generateUniqueInt64ID() {
  // Generate a 64-bit scope ID based on a UUID.
  std::string uuid = generateUUID();
  
  // Simple hash function to convert UUID string to 64-bit integer
  std::hash<std::string> hasher;
  size_t hash = hasher(uuid);
  
  // Convert to 64-bit and ensure it's not zero
  uint64_t scope_id = static_cast<uint64_t>(hash);
  if (scope_id == 0) {
    scope_id = 1;
  }

  return scope_id;
}

int getCurrentCallID(Module &M, bool increment) {
  if (NamedMDNode *NMD = M.getNamedMetadata("cats.trace.current_call_id")) {
    for (const MDNode *MD : NMD->operands()) {
      if (auto *MDV = dyn_cast<ConstantAsMetadata>(MD->getOperand(0))) {
        if (auto *CI = dyn_cast<ConstantInt>(MDV->getValue())) {
          int64_t ID = CI->getSExtValue();
          if (increment) {
            // Increment the call ID for the next use
            Constant *NewID = ConstantInt::get(
              Type::getInt64Ty(M.getContext()),
              ID + 1
            );
            NMD->clearOperands();
            NMD->addOperand(
              MDNode::get(M.getContext(), ConstantAsMetadata::get(NewID))
            );
          }
          return ID;
        }
      }
    }
  }

  // If no current call ID is found, return 0 and initialize it to 1 (if
  // increment is true)
  int initVal = increment ? 1 : 0;
  Constant *InitialID = ConstantInt::get(
    Type::getInt64Ty(M.getContext()), initVal
  );
  MDNode *InitialNode = MDNode::get(
    M.getContext(), ConstantAsMetadata::get(InitialID)
  );
  NamedMDNode *NMD = M.getOrInsertNamedMetadata("cats.trace.current_call_id");
  NMD->addOperand(InitialNode);
  return 0;
}

bool functionHasAnnotation(Function &F, StringRef Annotation) {
  // Look for llvm.global.annotations which stores
  // __attribute__((annotate(...)))
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
