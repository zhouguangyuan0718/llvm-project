#include "coredsl/ASTPrinter.h"
#include "coredsl/Diagnostics.h"
#include "coredsl/Parser.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

void printUsage(std::ostream &OS) {
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
      printUsage(std::cout);
      return 0;
    }
    if (!InputPath.empty()) {
      std::cerr << "error: expected exactly one input file\n";
      printUsage(std::cerr);
      return 1;
    }
    InputPath = Argument;
  }

  if (InputPath.empty()) {
    printUsage(std::cerr);
    return 1;
  }

  std::ifstream Input(InputPath);
  if (!Input) {
    std::cerr << "error: cannot open '" << InputPath << "'\n";
    return 1;
  }
  std::stringstream Buffer;
  Buffer << Input.rdbuf();

  coredsl::DiagnosticEngine Diags;
  coredsl::Parser Parser(InputPath, Buffer.str(), Diags);
  std::unique_ptr<coredsl::InstructionSetDecl> Decl =
      Parser.parseInstructionSet();
  Diags.print(std::cerr);
  if (!Decl || Diags.hasError())
    return 1;

  if (DumpAST)
    coredsl::printAST(*Decl, std::cout);
  return 0;
}
