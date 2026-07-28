#include "Tiny32TargetInfo.h"

#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

namespace llvm {
Target &getTheTiny32Target() {
  static Target TheTiny32Target;
  return TheTiny32Target;
}
} // namespace llvm

using namespace llvm;

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeTiny32TargetInfo() {
  TargetRegistry::RegisterTarget(
      getTheTiny32Target(), "tiny32", "Generated CoreDSL target", "Tiny32",
      [](Triple::ArchType) { return false; }, false);
}
