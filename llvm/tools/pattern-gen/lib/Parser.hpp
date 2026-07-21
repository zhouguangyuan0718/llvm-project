#pragma once
#include "InstrInfo.hpp"
#include "TokenStream.hpp"
#include <llvm/IR/Module.h>
#include <memory>

struct CoreDSLParserState;

/// Parses a CoreDSL instruction-set description into instruction metadata and
/// the LLVM IR behaviour functions consumed by PatternGen.
///
/// All mutable parser state belongs to this object.  The legacy free function
/// below remains as a source-compatible entry point for existing callers.
class CoreDSLParser {
public:
  CoreDSLParser(TokenStream &ts, bool is64Bit, llvm::Module &module,
                bool noExtend);
  ~CoreDSLParser();

  CoreDSLParser(const CoreDSLParser &) = delete;
  CoreDSLParser &operator=(const CoreDSLParser &) = delete;

  std::vector<CDSLInstr> parse();

private:
  TokenStream &TS;
  bool Is64Bit;
  llvm::Module &Module;
  bool NoExtend;

  std::unique_ptr<CoreDSLParserState> State;
};

std::vector<CDSLInstr> ParseCoreDSL2(TokenStream &ts, bool is64Bit,
                                     llvm::Module *mod, bool NoExtend);
