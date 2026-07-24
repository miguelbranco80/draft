// Shared LLVM target-registry initialization.

#include "backend/llvm_initialization.h"

#include <llvm-c/Target.h>

#include <mutex>

namespace draft {

void initialize_draft_llvm_targets() {
  static std::once_flag initialized;
  std::call_once(initialized, []() {
    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64Target();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64AsmParser();
    LLVMInitializeAArch64AsmPrinter();
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmParser();
    LLVMInitializeX86AsmPrinter();
  });
}

} // namespace draft
