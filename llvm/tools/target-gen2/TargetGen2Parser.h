//===- TargetGen2Parser.h - CoreDSL parser ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_TARGET_GEN2_PARSER_H
#define LLVM_TOOLS_TARGET_GEN2_PARSER_H

#include "TargetGen2AST.h"
#include "llvm/Support/Error.h"
#include "llvm/ADT/StringRef.h"
#include <memory>

namespace llvm {
namespace targetgen2 {

class Parser {
public:
  explicit Parser(StringRef Buffer);
  ~Parser();
  Expected<Description> parseDescription();

private:
  class Impl;
  std::unique_ptr<Impl> PImpl;
};

} // namespace targetgen2
} // namespace llvm

#endif
