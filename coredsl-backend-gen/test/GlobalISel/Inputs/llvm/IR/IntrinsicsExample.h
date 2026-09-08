// Test-only declaration for the example renderer's fictional target intrinsic.
// ICMP/BRCOND tests never construct or execute this intrinsic.
#ifndef COREDSL_TEST_INTRINSICS_EXAMPLE_H
#define COREDSL_TEST_INTRINSICS_EXAMPLE_H

#include "llvm/IR/Intrinsics.h"

namespace llvm::Intrinsic {
inline constexpr ID example_f16_op = static_cast<ID>(num_intrinsics);
}

#endif
