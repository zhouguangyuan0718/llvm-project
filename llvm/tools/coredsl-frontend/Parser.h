//===-- Parser.h - CoreDSL parser interface ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_COREDSL_FRONTEND_PARSER_H
#define LLVM_TOOLS_COREDSL_FRONTEND_PARSER_H

#include "CoreDSLIR.h"
#include "llvm/ADT/StringRef.h"

namespace llvm::coredsl {

/// Parse a CoreDSL source buffer and perform front-end semantic validation.
Module parseCoreDSL(StringRef Input);

} // namespace llvm::coredsl

#endif // LLVM_TOOLS_COREDSL_FRONTEND_PARSER_H
