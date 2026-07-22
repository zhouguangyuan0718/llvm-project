//===-- Main.cpp - CoreDSL front-end command-line driver -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "IRPrinter.h"
#include "Parser.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {

cl::opt<std::string> InputFilename(cl::Positional,
                                   cl::desc("<CoreDSL input file>"),
                                   cl::Required);
cl::opt<std::string> OutputFilename("o", cl::desc("Output filename"),
                                    cl::init("-"));

} // namespace

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv, "CoreDSL front-end\n");
  ErrorOr<std::unique_ptr<MemoryBuffer>> Input =
      MemoryBuffer::getFile(InputFilename);
  if (!Input) {
    errs() << "error: cannot read '" << InputFilename
           << "': " << Input.getError().message() << '\n';
    return 1;
  }

  std::error_code EC;
  raw_fd_ostream Output(OutputFilename, EC, sys::fs::OF_Text);
  if (EC) {
    errs() << "error: cannot open '" << OutputFilename << "': " << EC.message()
           << '\n';
    return 1;
  }

  coredsl::printIR(Output, coredsl::parseCoreDSL((*Input)->getBuffer()));
  return 0;
}
