#include "coredsl/ASTPrinter.h"
#include "coredsl/Diagnostics.h"
#include "coredsl/Parser.h"

#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <utility>

namespace {

void printUsage(llvm::raw_ostream &OS) {
  OS << "usage: coredsl-backend-gen [--dump-ast] <input.core_desc>\n";
}

} // namespace

int main(int argc, char **argv) {
  bool DumpAST = false;
  std::string InputPath;
  for (int I = 1; I < argc; ++I) {
    const std::string Argument = argv[I];
    if (Argument == "--dump-ast") {
      DumpAST = true;
      continue;
    }
    if (Argument == "--help" || Argument == "-h") {
      printUsage(llvm::outs());
      return 0;
    }
    if (!InputPath.empty()) {
      llvm::errs() << "error: expected exactly one input file\n";
      printUsage(llvm::errs());
      return 1;
    }
    InputPath = Argument;
  }

  if (InputPath.empty()) {
    printUsage(llvm::errs());
    return 1;
  }

  auto BufferOrError = llvm::MemoryBuffer::getFileOrSTDIN(InputPath);
  if (!BufferOrError) {
    llvm::errs() << "error: cannot open '" << InputPath
                 << "': " << BufferOrError.getError().message() << '\n';
    return 1;
  }

  llvm::SourceMgr Sources;
  const unsigned MainBuffer =
      Sources.AddNewSourceBuffer(std::move(*BufferOrError), llvm::SMLoc());
  coredsl::DiagnosticEngine Diags(Sources);
  coredsl::Parser Parser(Sources.getMemoryBuffer(MainBuffer)->getBuffer(),
                         Diags);
  std::unique_ptr<coredsl::InstructionSetDecl> Decl =
      Parser.parseInstructionSet();
  if (!Decl || Diags.hasError())
    return 1;

  if (DumpAST)
    coredsl::printAST(*Decl, llvm::outs());
  return 0;
}
