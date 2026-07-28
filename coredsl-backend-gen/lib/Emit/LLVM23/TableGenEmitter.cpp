#include "coredsl/TableGenEmitter.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringSwitch.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace coredsl {

namespace {

struct RegisterTensorType {
  std::string MVT;
  std::string RegisterClass;
  unsigned SizeInBits;
};

struct SelectionPattern {
  const InstructionDecl *Instruction;
  const OperandDecl *Output;
  const OperandDecl *LHS;
  const OperandDecl *RHS;
  RegisterTensorType Type;
  std::string Operator;
};

const OperandDecl *findOperand(const InstructionDecl &Instruction,
                               llvm::StringRef Name) {
  for (const OperandDecl &Operand : Instruction.Operands)
    if (Operand.Name == Name)
      return &Operand;
  return nullptr;
}

std::optional<unsigned> getIntegerLiteral(const Expr *Expression) {
  if (!Expression || Expression->kind() != Expr::Kind::Integer)
    return std::nullopt;
  unsigned Value = 0;
  if (llvm::StringRef(static_cast<const LiteralExpr *>(Expression)->Value)
          .getAsInteger(10, Value))
    return std::nullopt;
  return Value;
}

std::optional<std::pair<std::string, unsigned>>
getElementMVT(const TypeRef &Type) {
  if (!Type.isScalar())
    return std::nullopt;

  if (Type.Name == "signed" || Type.Name == "unsigned") {
    std::optional<unsigned> Width = getIntegerLiteral(Type.Width.get());
    if (!Width)
      return std::nullopt;
    return std::pair<std::string, unsigned>("i" + std::to_string(*Width),
                                            *Width);
  }

  return llvm::StringSwitch<std::optional<std::pair<std::string, unsigned>>>(
             Type.Name)
      .Case("fp16", std::pair<std::string, unsigned>("f16", 16))
      .Case("f16", std::pair<std::string, unsigned>("f16", 16))
      .Case("bf16", std::pair<std::string, unsigned>("bf16", 16))
      .Case("fp32", std::pair<std::string, unsigned>("f32", 32))
      .Case("f32", std::pair<std::string, unsigned>("f32", 32))
      .Case("fp64", std::pair<std::string, unsigned>("f64", 64))
      .Case("f64", std::pair<std::string, unsigned>("f64", 64))
      .Default(std::nullopt);
}

std::optional<RegisterTensorType> getRegisterTensorType(const TypeRef &Type) {
  if (!Type.isTensor() || Type.Storage != TypeRef::TensorStorage::Register ||
      Type.Shape.size() != 1 || !Type.ElementType)
    return std::nullopt;

  std::optional<unsigned> Elements = getIntegerLiteral(Type.Shape[0].get());
  auto Element = getElementMVT(*Type.ElementType);
  if (!Elements || !Element)
    return std::nullopt;

  RegisterTensorType Result;
  Result.MVT = "v" + std::to_string(*Elements) + Element->first;
  Result.RegisterClass = "TensorReg_" + Result.MVT;
  Result.SizeInBits = *Elements * Element->second;
  return Result;
}

const Expr *resolveLocal(const Expr *Expression,
                         const llvm::StringMap<const Expr *> &Locals) {
  llvm::StringSet<> Visited;
  while (Expression && Expression->kind() == Expr::Kind::Identifier) {
    llvm::StringRef Name =
        static_cast<const IdentifierExpr *>(Expression)->Name;
    auto Local = Locals.find(Name);
    if (Local == Locals.end() || !Visited.insert(Name).second)
      break;
    Expression = Local->second;
  }
  return Expression;
}

std::optional<SelectionPattern>
inferSelectionPattern(const InstructionDecl &Instruction) {
  if (!Instruction.Behavior ||
      Instruction.Behavior->kind() != Stmt::Kind::Compound)
    return std::nullopt;

  llvm::StringMap<const Expr *> Locals;
  const OperandDecl *Output = nullptr;
  const Expr *OutputExpression = nullptr;
  for (const auto &Statement :
       static_cast<const CompoundStmt *>(Instruction.Behavior.get())
           ->Statements) {
    if (Statement->kind() == Stmt::Kind::Decl) {
      const auto *Declaration = static_cast<const DeclStmt *>(Statement.get());
      if (Declaration->Initializer)
        Locals[Declaration->Name] = Declaration->Initializer.get();
      continue;
    }
    if (Statement->kind() != Stmt::Kind::Expr)
      continue;
    const Expr *Expression =
        static_cast<const ExprStmt *>(Statement.get())->Expression.get();
    if (Expression->kind() != Expr::Kind::Binary)
      continue;
    const auto *Assignment = static_cast<const BinaryExpr *>(Expression);
    if (Assignment->Operator != "=" ||
        Assignment->LHS->kind() != Expr::Kind::Identifier)
      continue;
    llvm::StringRef Name =
        static_cast<const IdentifierExpr *>(Assignment->LHS.get())->Name;
    const OperandDecl *Candidate = findOperand(Instruction, Name);
    if (!Candidate)
      continue;
    Output = Candidate;
    OutputExpression = resolveLocal(Assignment->RHS.get(), Locals);
  }

  if (!Output || !OutputExpression ||
      OutputExpression->kind() != Expr::Kind::Binary)
    return std::nullopt;

  const auto *Operation = static_cast<const BinaryExpr *>(OutputExpression);
  std::string Operator = llvm::StringSwitch<std::string>(Operation->Operator)
                             .Case("+", "add")
                             .Case("-", "sub")
                             .Case("*", "mul")
                             .Case("&", "and")
                             .Case("|", "or")
                             .Case("^", "xor")
                             .Default("");
  if (Operator.empty())
    return std::nullopt;

  const Expr *LHS = resolveLocal(Operation->LHS.get(), Locals);
  const Expr *RHS = resolveLocal(Operation->RHS.get(), Locals);
  if (!LHS || !RHS || LHS->kind() != Expr::Kind::Identifier ||
      RHS->kind() != Expr::Kind::Identifier)
    return std::nullopt;

  const OperandDecl *LHSOperand =
      findOperand(Instruction, static_cast<const IdentifierExpr *>(LHS)->Name);
  const OperandDecl *RHSOperand =
      findOperand(Instruction, static_cast<const IdentifierExpr *>(RHS)->Name);
  if (!LHSOperand || !RHSOperand)
    return std::nullopt;

  auto OutputType = getRegisterTensorType(Output->Type);
  auto LHSType = getRegisterTensorType(LHSOperand->Type);
  auto RHSType = getRegisterTensorType(RHSOperand->Type);
  if (!OutputType || !LHSType || !RHSType || OutputType->MVT != LHSType->MVT ||
      OutputType->MVT != RHSType->MVT)
    return std::nullopt;

  return SelectionPattern{&Instruction,
                          Output,
                          LHSOperand,
                          RHSOperand,
                          std::move(*OutputType),
                          std::move(Operator)};
}

void emitRegisterScaffold(llvm::raw_ostream &OS) {
  OS << "// Spike-only synthetic register file. Real register names, count, "
        "encodings,\n";
  OS << "// aliases and allocation properties still require target "
        "description data.\n";
  for (unsigned I = 0; I != 4; ++I)
    OS << "def TR" << I << " : Register<\"tr" << I << "\">;\n";
  OS << "def X0 : Register<\"x0\">;\n\n";
}

void emitInstruction(const SelectionPattern &Pattern, llvm::raw_ostream &OS) {
  const InstructionDecl &Instruction = *Pattern.Instruction;
  OS << "// " << Instruction.Name
     << " remains pseudo in this spike: a final EncodingModel and target "
        "instruction\n";
  OS << "// format class are not available yet.\n";
  OS << "def " << Instruction.Name << "\n";
  OS << "    : CoreDSLSpikeInst<(outs " << Pattern.Type.RegisterClass << ":$"
     << Pattern.Output->Name << "),\n";
  OS << "                       (ins " << Pattern.Type.RegisterClass << ":$"
     << Pattern.LHS->Name << ", " << Pattern.Type.RegisterClass << ":$"
     << Pattern.RHS->Name << "),\n";
  OS << "                       \"" << Instruction.Name << " $"
     << Pattern.Output->Name << ", $" << Pattern.LHS->Name << ", $"
     << Pattern.RHS->Name << "\">;\n\n";

  OS << "def : Pat<(" << Pattern.Type.MVT << " (" << Pattern.Operator << " ("
     << Pattern.Type.MVT << " " << Pattern.Type.RegisterClass << ":$"
     << Pattern.LHS->Name << "),\n";
  OS << "                                      (" << Pattern.Type.MVT << " "
     << Pattern.Type.RegisterClass << ":$" << Pattern.RHS->Name << "))),\n";
  OS << "          (" << Instruction.Name << " " << Pattern.Type.RegisterClass
     << ":$" << Pattern.LHS->Name << ", " << Pattern.Type.RegisterClass << ":$"
     << Pattern.RHS->Name << ")>;\n\n";
}

std::string getUnsupportedReason(const InstructionDecl &Instruction) {
  if (Instruction.Operands.empty())
    return "typed operand declarations are missing";
  for (const OperandDecl &Operand : Instruction.Operands)
    if (Operand.Type.isTensor() &&
        Operand.Type.Storage == TypeRef::TensorStorage::Memory)
      return "memory tensor needs pointer, dynamic extent/stride ABI, memory "
             "effects and a selection source";
  return "behavior is outside the fixed-vector binary-operation spike subset";
}

} // namespace

bool emitTableGenSpike(const InstructionSetDecl &InstructionSet,
                       llvm::raw_ostream &OS) {
  std::vector<SelectionPattern> Patterns;
  for (const InstructionDecl &Instruction : InstructionSet.Instructions)
    if (auto Pattern = inferSelectionPattern(Instruction))
      Patterns.push_back(std::move(*Pattern));

  OS << "// Generated CoreDSL LLVM 23 TableGen spike.\n";
  OS << "// This validation unit intentionally uses pseudo instructions and a "
        "synthetic\n";
  OS << "// register file; it proves def/Pat import, not final MC encoding.\n";
  OS << "include \"llvm/Target/Target.td\"\n\n";
  emitRegisterScaffold(OS);

  llvm::StringSet<> EmittedClasses;
  for (const SelectionPattern &Pattern : Patterns) {
    if (!EmittedClasses.insert(Pattern.Type.RegisterClass).second)
      continue;
    OS << "def " << Pattern.Type.RegisterClass
       << " : RegisterClass<\"CoreDSLSpike\", [" << Pattern.Type.MVT << "], "
       << Pattern.Type.SizeInBits << ", (add TR0, TR1, TR2, TR3)>;\n";
  }
  OS << "def GPR : RegisterClass<\"CoreDSLSpike\", [i64], 64, (add X0)>;\n\n";

  OS << "class CoreDSLSpikeInst<dag outs, dag ins, string asmstr> : "
        "Instruction {\n";
  OS << "  let Namespace = \"CoreDSLSpike\";\n";
  OS << "  let OutOperandList = outs;\n";
  OS << "  let InOperandList = ins;\n";
  OS << "  let AsmString = asmstr;\n";
  OS << "  let Pattern = [];\n";
  OS << "  let hasSideEffects = 0;\n";
  OS << "  let mayLoad = 0;\n";
  OS << "  let mayStore = 0;\n";
  OS << "  let isPseudo = 1;\n";
  OS << "}\n\n";

  for (const SelectionPattern &Pattern : Patterns)
    emitInstruction(Pattern, OS);

  for (const InstructionDecl &Instruction : InstructionSet.Instructions) {
    bool Emitted = false;
    for (const SelectionPattern &Pattern : Patterns)
      Emitted |= Pattern.Instruction == &Instruction;
    if (!Emitted)
      OS << "// Not emitted: " << Instruction.Name << ": "
         << getUnsupportedReason(Instruction) << ".\n";
  }
  OS << "\ndefm : RemapAllTargetPseudoPointerOperands<GPR>;\n\n";
  OS << "def CoreDSLSpikeInstrInfo : InstrInfo;\n";
  OS << "def CoreDSLSpike : Target {\n";
  OS << "  let InstructionSet = CoreDSLSpikeInstrInfo;\n";
  OS << "}\n";
  return !Patterns.empty();
}

} // namespace coredsl
