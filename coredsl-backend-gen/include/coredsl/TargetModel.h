#ifndef COREDSL_TARGETMODEL_H
#define COREDSL_TARGETMODEL_H

#include "coredsl/AST.h"

#include <optional>
#include <string>
#include <vector>

namespace llvm {
class raw_ostream;
}

namespace coredsl {

/// LLVM-independent, validated target facts.  This is the ownership boundary
/// between the CoreDSL frontend and every backend emitter.
struct TargetConfigModel {
  std::string InstructionSetName;
  std::string LLVMName;
  std::string Namespace;
  std::string RegisterPrefix;
  unsigned RegisterCount = 0;
  unsigned RegisterWidth = 0;
  std::string RegisterClass;
  std::string RegisterBank;
};

struct ValueTypeModel {
  enum class Kind { ScalarInteger, FixedVectorInteger };

  Kind K = Kind::ScalarInteger;
  unsigned ScalarBits = 0;
  unsigned Elements = 1;

  std::string tableGenType() const;
};

struct OperandModel {
  std::string Name;
  ValueTypeModel Type;
  SourceRange Range;
};

enum class GenericOpcode {
  Add,
  Sub,
  Mul,
  And,
  Or,
  Xor,
  Shl,
  LShr,
  AShr,
};

const char *genericOpcodeName(GenericOpcode Opcode);

/// A three-register, single-result Generic MIR operation selected by one
/// target instruction.  Absent rules are intentional: the instruction still
/// belongs to the target model but does not claim GlobalISel coverage.
struct SelectionRuleModel {
  GenericOpcode Opcode;
  unsigned OutputOperand = 0;
  unsigned LHSOperand = 0;
  unsigned RHSOperand = 0;
};

struct InstructionModel {
  std::string Name;
  std::vector<OperandModel> Operands;
  std::optional<SelectionRuleModel> SelectionRule;
  SourceRange Range;
};

struct TargetModel {
  TargetConfigModel Config;
  std::vector<InstructionModel> Instructions;
};

/// Performs semantic lowering from source-preserving AST to canonical model.
/// It never invokes LLVM IR or CodeGen APIs.
std::optional<TargetModel> buildTargetModel(const InstructionSetDecl &Decl,
                                            DiagnosticEngine &Diags);

void printTargetModel(const TargetModel &Model, llvm::raw_ostream &OS);

} // namespace coredsl

#endif // COREDSL_TARGETMODEL_H
