//===-- IRPrinter.h - CDSLIR textual printer -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_COREDSL_FRONTEND_IRPRINTER_H
#define LLVM_TOOLS_COREDSL_FRONTEND_IRPRINTER_H

#include "CoreDSLIR.h"

namespace llvm {
class raw_ostream;
}

namespace llvm::coredsl {

void printIR(raw_ostream &OS, const Module &M);

} // namespace llvm::coredsl

#endif // LLVM_TOOLS_COREDSL_FRONTEND_IRPRINTER_H
