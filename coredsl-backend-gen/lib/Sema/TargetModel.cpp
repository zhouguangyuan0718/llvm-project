#include "coredsl/TargetModel.h"

#include "coredsl/Diagnostics.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/raw_ostream.h"

#include <cctype>
#include <optional>
#include <string>
#include <utility>

namespace coredsl {

namespace {

std::optional<unsigned> getUnsignedInteger(const Expr &Expression) {
  if (Expression.kind() != Expr::Kind::Integer)
    return std::nullopt;
  unsigned Value = 0;
  if (llvm::StringRef(static_cast<const LiteralExpr &>(Expression).Value)
          .getAsInteger(10, Value))
    return std::nullopt;
  return Value;
}

std::optional<std::string> getStringOrIdentifier(const Expr &Expression) {
  if (Expression.kind() == Expr::Kind::Identifier)
    return static_cast<const IdentifierExpr &>(Expression).Name;
  if (Expression.kind() == Expr::Kind::String) {
    llvm::StringRef Spelling =
        static_cast<const LiteralExpr &>(Expression).Value;
    if (Spelling.size() < 2 || Spelling.front() != '"' ||
        Spelling.back() != '"')
      return std::nullopt;
    return Spelling.drop_front().drop_back().str();
  }
  return std::nullopt;
}

bool isIdentifier(llvm::StringRef Name) {
  if (Name.empty() ||
      !(std::isalpha(static_cast<unsigned char>(Name.front())) ||
        Name.front() == '_'))
    return false;
  for (char C : Name.drop_front())
    if (!(std::isalnum(static_cast<unsigned char>(C)) || C == '_'))
      return false;
  return true;
}

const TargetPropertyDecl *findProperty(const TargetDecl &Target,
                                       llvm::StringRef Name,
                                       DiagnosticEngine &Diags) {
  const TargetPropertyDecl *Result = nullptr;
  for (const TargetPropertyDecl &Property : Target.Properties) {
    if (Property.Name != Name)
      continue;
    if (Result) {
      Diags.error(Property.Range.Begin,
                  "duplicate target property '" + Name.str() + "'");
      continue;
    }
    Result = &Property;
  }
  if (!Result)
    Diags.error(Target.Range.Begin,
                "missing required target property '" + Name.str() + "'");
  return Result;
}

std::optional<ValueTypeModel> lowerType(const TypeRef &Type,
                                        DiagnosticEngine &Diags) {
  const TypeRef *ElementType = &Type;
  unsigned Elements = 1;
  if (Type.isTensor()) {
    if (Type.Storage != TypeRef::TensorStorage::Register ||
        Type.Shape.size() != 1 || !Type.ElementType) {
      Diags.error(Type.Range.Begin,
                  "only fixed one-dimensional register tensors are supported "
                  "by the initial GlobalISel model");
      return std::nullopt;
    }
    std::optional<unsigned> Shape = getUnsignedInteger(*Type.Shape.front());
    if (!Shape || *Shape == 0) {
      Diags.error(Type.Shape.front()->range().Begin,
                  "register tensor element count must be a positive integer");
      return std::nullopt;
    }
    Elements = *Shape;
    ElementType = Type.ElementType.get();
  }

  if (!ElementType->isScalar() ||
      (ElementType->Name != "signed" && ElementType->Name != "unsigned" &&
       ElementType->Name != "i")) {
    Diags.error(ElementType->Range.Begin,
                "initial GlobalISel model requires signed<N>, unsigned<N>, "
                "or i<N> operand types");
    return std::nullopt;
  }
  std::optional<unsigned> Bits = ElementType->Width
                                     ? getUnsignedInteger(*ElementType->Width)
                                     : std::nullopt;
  if (!Bits || (*Bits != 8 && *Bits != 16 && *Bits != 32 && *Bits != 64)) {
    Diags.error(ElementType->Range.Begin,
                "initial GlobalISel model supports integer widths 8, 16, 32, "
                "and 64");
    return std::nullopt;
  }
  return ValueTypeModel{Elements == 1
                            ? ValueTypeModel::Kind::ScalarInteger
                            : ValueTypeModel::Kind::FixedVectorInteger,
                        *Bits, Elements};
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

const OperandModel *findOperand(const InstructionModel &Instruction,
                                llvm::StringRef Name, unsigned &Index) {
  for (unsigned I = 0; I != Instruction.Operands.size(); ++I) {
    if (Instruction.Operands[I].Name == Name) {
      Index = I;
      return &Instruction.Operands[I];
    }
  }
  return nullptr;
}

std::optional<GenericOpcode> lowerGenericOpcode(llvm::StringRef Operator) {
  return llvm::StringSwitch<std::optional<GenericOpcode>>(Operator)
      .Case("+", GenericOpcode::Add)
      .Case("-", GenericOpcode::Sub)
      .Case("*", GenericOpcode::Mul)
      .Case("&", GenericOpcode::And)
      .Case("|", GenericOpcode::Or)
      .Case("^", GenericOpcode::Xor)
      .Case("<<", GenericOpcode::Shl)
      .Case(">>u", GenericOpcode::LShr)
      .Case(">>s", GenericOpcode::AShr)
      .Default(std::nullopt);
}

std::optional<SelectionRuleModel>
inferSelectionRule(const InstructionDecl &Decl,
                   const InstructionModel &Instruction) {
  if (!Decl.Behavior || Decl.Behavior->kind() != Stmt::Kind::Compound)
    return std::nullopt;

  llvm::StringMap<const Expr *> Locals;
  const Expr *OutputExpression = nullptr;
  unsigned OutputIndex = 0;
  bool FoundOutput = false;
  for (const std::unique_ptr<Stmt> &Statement :
       static_cast<const CompoundStmt &>(*Decl.Behavior).Statements) {
    if (Statement->kind() == Stmt::Kind::Decl) {
      const auto &Local = static_cast<const DeclStmt &>(*Statement);
      if (Local.Initializer)
        Locals[Local.Name] = Local.Initializer.get();
      continue;
    }
    if (Statement->kind() != Stmt::Kind::Expr)
      continue;
    const Expr *Expression =
        static_cast<const ExprStmt &>(*Statement).Expression.get();
    if (Expression->kind() != Expr::Kind::Binary)
      continue;
    const auto &Assignment = static_cast<const BinaryExpr &>(*Expression);
    if (Assignment.Operator != "=" ||
        Assignment.LHS->kind() != Expr::Kind::Identifier)
      continue;
    unsigned CandidateIndex = 0;
    if (!findOperand(Instruction,
                     static_cast<const IdentifierExpr &>(*Assignment.LHS).Name,
                     CandidateIndex))
      continue;
    if (FoundOutput)
      return std::nullopt;
    FoundOutput = true;
    OutputIndex = CandidateIndex;
    OutputExpression = resolveLocal(Assignment.RHS.get(), Locals);
  }

  if (!FoundOutput || !OutputExpression ||
      OutputExpression->kind() != Expr::Kind::Binary)
    return std::nullopt;
  const auto &Operation = static_cast<const BinaryExpr &>(*OutputExpression);
  std::optional<GenericOpcode> Opcode = lowerGenericOpcode(Operation.Operator);
  if (!Opcode)
    return std::nullopt;
  const Expr *LHS = resolveLocal(Operation.LHS.get(), Locals);
  const Expr *RHS = resolveLocal(Operation.RHS.get(), Locals);
  if (!LHS || !RHS || LHS->kind() != Expr::Kind::Identifier ||
      RHS->kind() != Expr::Kind::Identifier)
    return std::nullopt;
  unsigned LHSIndex = 0;
  unsigned RHSIndex = 0;
  const OperandModel *LHSOperand = findOperand(
      Instruction, static_cast<const IdentifierExpr &>(*LHS).Name, LHSIndex);
  const OperandModel *RHSOperand = findOperand(
      Instruction, static_cast<const IdentifierExpr &>(*RHS).Name, RHSIndex);
  if (!LHSOperand || !RHSOperand ||
      Instruction.Operands[OutputIndex].Type.tableGenType() !=
          LHSOperand->Type.tableGenType() ||
      Instruction.Operands[OutputIndex].Type.tableGenType() !=
          RHSOperand->Type.tableGenType())
    return std::nullopt;
  return SelectionRuleModel{*Opcode, OutputIndex, LHSIndex, RHSIndex};
}

} // namespace

std::string ValueTypeModel::tableGenType() const {
  const std::string Element = "i" + std::to_string(ScalarBits);
  if (K == Kind::ScalarInteger)
    return Element;
  return "v" + std::to_string(Elements) + Element;
}

const char *genericOpcodeName(GenericOpcode Opcode) {
  switch (Opcode) {
  case GenericOpcode::Add:
    return "G_ADD";
  case GenericOpcode::Sub:
    return "G_SUB";
  case GenericOpcode::Mul:
    return "G_MUL";
  case GenericOpcode::And:
    return "G_AND";
  case GenericOpcode::Or:
    return "G_OR";
  case GenericOpcode::Xor:
    return "G_XOR";
  case GenericOpcode::Shl:
    return "G_SHL";
  case GenericOpcode::LShr:
    return "G_LSHR";
  case GenericOpcode::AShr:
    return "G_ASHR";
  }
  return "G_UNKNOWN";
}

std::optional<TargetModel> buildTargetModel(const InstructionSetDecl &Decl,
                                            DiagnosticEngine &Diags) {
  if (!Decl.Target) {
    Diags.error(Decl.Range.Begin,
                "backend generation requires a 'target' block");
    return std::nullopt;
  }

  const TargetDecl &Target = *Decl.Target;
  TargetModel Result;
  Result.Config.InstructionSetName = Decl.Name;
  auto getText = [&](llvm::StringRef Name, std::string &Output) {
    const TargetPropertyDecl *Property = findProperty(Target, Name, Diags);
    if (!Property)
      return;
    std::optional<std::string> Value = getStringOrIdentifier(*Property->Value);
    if (!Value) {
      Diags.error(Property->Value->range().Begin,
                  "target property '" + Name.str() +
                      "' requires an identifier or string value");
      return;
    }
    Output = std::move(*Value);
  };
  auto getUnsigned = [&](llvm::StringRef Name, unsigned &Output) {
    const TargetPropertyDecl *Property = findProperty(Target, Name, Diags);
    if (!Property)
      return;
    std::optional<unsigned> Value = getUnsignedInteger(*Property->Value);
    if (!Value || *Value == 0) {
      Diags.error(Property->Value->range().Begin,
                  "target property '" + Name.str() +
                      "' requires a positive integer value");
      return;
    }
    Output = *Value;
  };
  getText("llvm_name", Result.Config.LLVMName);
  getText("namespace", Result.Config.Namespace);
  getText("register_prefix", Result.Config.RegisterPrefix);
  getUnsigned("register_count", Result.Config.RegisterCount);
  getUnsigned("register_width", Result.Config.RegisterWidth);
  getText("register_class", Result.Config.RegisterClass);
  getText("register_bank", Result.Config.RegisterBank);
  for (const std::pair<llvm::StringRef, const std::string *> Property :
       {std::pair<llvm::StringRef, const std::string *>{
            "namespace", &Result.Config.Namespace},
        {"register_class", &Result.Config.RegisterClass},
        {"register_bank", &Result.Config.RegisterBank}})
    if (!isIdentifier(*Property.second))
      Diags.error(Target.Range.Begin,
                  "target property '" + Property.first.str() +
                      "' must be a C++/TableGen identifier");
  if (Result.Config.RegisterWidth != 8 && Result.Config.RegisterWidth != 16 &&
      Result.Config.RegisterWidth != 32 && Result.Config.RegisterWidth != 64)
    Diags.error(Target.Range.Begin,
                "initial GlobalISel model supports register widths 8, 16, 32, "
                "and 64");

  for (const InstructionDecl &DeclInstruction : Decl.Instructions) {
    InstructionModel Instruction;
    Instruction.Name = DeclInstruction.Name;
    Instruction.Range = DeclInstruction.Range;
    llvm::StringSet<> Names;
    for (const OperandDecl &Operand : DeclInstruction.Operands) {
      if (!Names.insert(Operand.Name).second) {
        Diags.error(Operand.Range.Begin,
                    "duplicate operand '" + Operand.Name + "'");
        continue;
      }
      std::optional<ValueTypeModel> Type = lowerType(Operand.Type, Diags);
      if (!Type)
        continue;
      Instruction.Operands.push_back(
          {Operand.Name, std::move(*Type), Operand.Range});
    }
    Instruction.SelectionRule =
        inferSelectionRule(DeclInstruction, Instruction);
    Result.Instructions.push_back(std::move(Instruction));
  }

  if (Diags.hasError())
    return std::nullopt;
  return Result;
}

void printTargetModel(const TargetModel &Model, llvm::raw_ostream &OS) {
  const TargetConfigModel &Config = Model.Config;
  OS << "target " << Config.LLVMName << " namespace " << Config.Namespace
     << "\n";
  OS << "  registers " << Config.RegisterClass << " bank "
     << Config.RegisterBank << " width " << Config.RegisterWidth << " count "
     << Config.RegisterCount << " prefix " << Config.RegisterPrefix << "\n";
  for (const InstructionModel &Instruction : Model.Instructions) {
    OS << "  instruction " << Instruction.Name << "\n";
    for (const OperandModel &Operand : Instruction.Operands)
      OS << "    operand " << Operand.Name << " " << Operand.Type.tableGenType()
         << "\n";
    if (Instruction.SelectionRule) {
      const SelectionRuleModel &Rule = *Instruction.SelectionRule;
      OS << "    select " << genericOpcodeName(Rule.Opcode) << " "
         << Instruction.Operands[Rule.OutputOperand].Name << " <- "
         << Instruction.Operands[Rule.LHSOperand].Name << ", "
         << Instruction.Operands[Rule.RHSOperand].Name << "\n";
    } else {
      OS << "    select unavailable\n";
    }
  }
}

} // namespace coredsl
