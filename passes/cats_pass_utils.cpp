#include "cats_passes.hpp"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

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
