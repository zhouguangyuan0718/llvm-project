#include "coredsl/ASTPrinter.h"
#include "coredsl/Diagnostics.h"
#include "coredsl/LLVM23Emitter.h"
#include "coredsl/Parser.h"
#include "coredsl/TableGenEmitter.h"
#include "coredsl/TargetModel.h"

#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <utility>

namespace {

void printUsage(llvm::raw_ostream &OS) {
  OS << "usage: coredsl-backend-gen [--dump-ast | --dump-model | "
        "--emit-td-spike | "
        "--emit-llvm23-td | --emit-llvm23-backend=<directory>] "
        "<input.core_desc>\n";
}

} // namespace

int main(int argc, char **argv) {
  bool DumpAST = false;
  bool DumpModel = false;
  bool EmitTDSpike = false;
  bool EmitLLVM23TD = false;
  std::string EmitLLVM23BackendDirectory;
  std::string InputPath;
  for (int I = 1; I < argc; ++I) {
    const std::string Argument = argv[I];
    if (Argument == "--dump-ast") {
      DumpAST = true;
      continue;
    }
    if (Argument == "--dump-model") {
      DumpModel = true;
      continue;
    }
    if (Argument == "--emit-td-spike") {
      EmitTDSpike = true;
      continue;
    }
    if (Argument == "--emit-llvm23-td") {
      EmitLLVM23TD = true;
      continue;
    }
    constexpr llvm::StringLiteral EmitBackendPrefix = "--emit-llvm23-backend=";
    if (llvm::StringRef(Argument).starts_with(EmitBackendPrefix)) {
      EmitLLVM23BackendDirectory =
          llvm::StringRef(Argument).drop_front(EmitBackendPrefix.size()).str();
      if (EmitLLVM23BackendDirectory.empty()) {
        llvm::errs() << "error: --emit-llvm23-backend requires a directory\n";
        return 1;
      }
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
  const unsigned OutputModeCount = DumpAST + DumpModel + EmitTDSpike +
                                   EmitLLVM23TD +
                                   !EmitLLVM23BackendDirectory.empty();
  if (OutputModeCount > 1) {
    llvm::errs() << "error: output mode options are mutually exclusive\n";
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
  if (DumpModel) {
    std::optional<coredsl::TargetModel> Model =
        coredsl::buildTargetModel(*Decl, Diags);
    if (!Model || Diags.hasError())
      return 1;
    coredsl::printTargetModel(*Model, llvm::outs());
  }
  if (EmitTDSpike && !coredsl::emitTableGenSpike(*Decl, llvm::outs()))
    return 1;
  if (EmitLLVM23TD) {
    std::optional<coredsl::TargetModel> Model =
        coredsl::buildTargetModel(*Decl, Diags);
    if (!Model || Diags.hasError())
      return 1;
    coredsl::emitLLVM23TableGen(*Model, llvm::outs());
  }
  if (!EmitLLVM23BackendDirectory.empty()) {
    std::optional<coredsl::TargetModel> Model =
        coredsl::buildTargetModel(*Decl, Diags);
    if (!Model || Diags.hasError())
      return 1;
    if (!coredsl::emitLLVM23Backend(*Model, EmitLLVM23BackendDirectory,
                                    llvm::errs()))
      return 1;
  }
  return 0;
}
