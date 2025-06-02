#include "cats_passes.hpp"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

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
