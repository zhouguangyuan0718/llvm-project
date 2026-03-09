//===- target-gen2.cpp - CoreDSL frontend entry point -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TargetGen2IR.h"
#include "TargetGen2JSON.h"
#include "TargetGen2Parser.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::targetgen2;

static cl::OptionCategory TargetGenCategory("target-gen2 options");

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input CoreDSL file>"),
                                          cl::Required,
                                          cl::cat(TargetGenCategory));

static cl::opt<std::string>
    OutputFilename("o", cl::desc("Output filename"), cl::init("-"),
                   cl::value_desc("filename"), cl::cat(TargetGenCategory));

static cl::opt<std::string>
    EmitFormat("emit", cl::desc("Output format"), cl::init("json"),
               cl::value_desc("json|llvm-ir"), cl::cat(TargetGenCategory));

int main(int argc, const char **argv) {
  InitLLVM X(argc, argv);
  cl::HideUnrelatedOptions({&TargetGenCategory});
  cl::ParseCommandLineOptions(argc, argv, "CoreDSL frontend\n");

  ErrorOr<std::unique_ptr<MemoryBuffer>> BufferOrErr =
      MemoryBuffer::getFile(InputFilename);
  if (!BufferOrErr) {
    errs() << "target-gen2: unable to read '" << InputFilename
           << "': " << BufferOrErr.getError().message() << '\n';
    return 1;
  }

  Parser P(BufferOrErr.get()->getBuffer());
  Expected<Description> DescOrErr = P.parseDescription();
  if (!DescOrErr) {
    errs() << "target-gen2: parse failed:\n" << toString(DescOrErr.takeError());
    return 1;
  }

  std::error_code EC;
  raw_fd_ostream OS(OutputFilename, EC, sys::fs::OF_Text);
  if (EC) {
    errs() << "target-gen2: unable to open output '" << OutputFilename
           << "': " << EC.message() << '\n';
    return 1;
  }

  if (EmitFormat == "json") {
    OS << formatv("{0:2}\n", toJSON(*DescOrErr));
  } else if (EmitFormat == "llvm-ir") {
    OS << toLLVMIR(*DescOrErr);
  } else {
    errs() << "target-gen2: unsupported --emit value '" << EmitFormat
           << "' (expected json or llvm-ir)\n";
    return 1;
  }
  return 0;
}
