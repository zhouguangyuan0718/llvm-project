//===- TargetGen2JSON.cpp - CoreDSL JSON emission ---------------*- C++ -*-===//

#include "TargetGen2JSON.h"

using namespace llvm;
using namespace llvm::targetgen2;

namespace detail {

StringRef toString(ExprKind K) {
  switch (K) {
  case ExprKind::Unknown: return "unknown";
  case ExprKind::Assignment: return "assignment";
  case ExprKind::Conditional: return "conditional";
  case ExprKind::Binary: return "binary";
  case ExprKind::Unary: return "unary";
  case ExprKind::Cast: return "cast";
  case ExprKind::Index: return "index";
  case ExprKind::Call: return "call";
  case ExprKind::Member: return "member";
  case ExprKind::Postfix: return "postfix";
  case ExprKind::Identifier: return "identifier";
  case ExprKind::Literal: return "literal";
  case ExprKind::Group: return "group";
  }
  return "unknown";
}

StringRef toString(StatementKind K) {
  switch (K) {
  case StatementKind::Empty: return "empty";
  case StatementKind::Compound: return "compound";
  case StatementKind::If: return "if";
  case StatementKind::Switch: return "switch";
  case StatementKind::Case: return "case";
  case StatementKind::Default: return "default";
  case StatementKind::While: return "while";
  case StatementKind::For: return "for";
  case StatementKind::DoWhile: return "do-while";
  case StatementKind::Spawn: return "spawn";
  case StatementKind::Continue: return "continue";
  case StatementKind::Break: return "break";
  case StatementKind::Return: return "return";
  case StatementKind::Expression: return "expression";
  }
  return "expression";
}

json::Value toJSON(const EncodingField &Field) {
  if (Field.IsBitValue)
    return json::Object{{"kind", "bit_value"}, {"value", Field.Value}};
  return json::Object{{"kind", "bit_field"},
                      {"name", Field.Name},
                      {"start", Field.StartBit},
                      {"end", Field.EndBit}};
}

json::Value toJSON(const Assembly &Asm) {
  if (!Asm.IsStructured)
    return json::Object{{"kind", "string"}, {"assembly", Asm.Template}};
  return json::Object{{"kind", "structured"},
                      {"mnemonic", Asm.Mnemonic},
                      {"assembly", Asm.Template}};
}

json::Value toJSON(const Statement &Stmt) {
  auto ToExprJSON = [](const Expr &E, const auto &Self) -> json::Value {
    json::Array Kids;
    for (const Expr &C : E.Children)
      Kids.push_back(Self(C, Self));
    return json::Object{{"kind", toString(E.Kind)},
                        {"text", E.Text},
                        {"op", E.Op},
                        {"value", E.Value},
                        {"children", std::move(Kids)}};
  };

  json::Array Exprs;
  for (const Expr &E : Stmt.Expressions)
    Exprs.push_back(ToExprJSON(E, ToExprJSON));

  json::Array Children;
  for (const Statement &Child : Stmt.Children)
    Children.push_back(toJSON(Child));
  return json::Object{{"kind", toString(Stmt.Kind)},
                      {"text", Stmt.Text},
                      {"expressions", std::move(Exprs)},
                      {"children", std::move(Children)}};
}

json::Value toJSON(const Instruction &Inst) {
  json::Array Attrs;
  for (const std::string &A : Inst.Attributes)
    Attrs.push_back(A);

  json::Array Enc;
  for (const EncodingField &F : Inst.Encoding)
    Enc.push_back(toJSON(F));

  json::Object Obj{{"name", Inst.Name},
                   {"attributes", std::move(Attrs)},
                   {"encoding", std::move(Enc)},
                   {"behavior", toJSON(Inst.Behavior)}};
  if (Inst.Asm)
    Obj["assembly"] = toJSON(*Inst.Asm);
  return Obj;
}

json::Value toJSON(const AlwaysBlock &Block) {
  json::Array Attrs;
  for (const std::string &A : Block.Attributes)
    Attrs.push_back(A);

  return json::Object{{"name", Block.Name},
                      {"attributes", std::move(Attrs)},
                      {"behavior", toJSON(Block.Behavior)}};
}

json::Value toJSON(const ISASections &ISA) {
  json::Object Obj;
  if (ISA.ArchitecturalState)
    Obj["architectural_state"] = *ISA.ArchitecturalState;
  if (ISA.Functions)
    Obj["functions"] = *ISA.Functions;

  json::Array InstAttrs;
  for (const std::string &A : ISA.CommonInstructionAttributes)
    InstAttrs.push_back(A);
  if (!InstAttrs.empty())
    Obj["instructions_attributes"] = std::move(InstAttrs);

  json::Array Instructions;
  for (const Instruction &I : ISA.Instructions)
    Instructions.push_back(toJSON(I));
  if (!Instructions.empty())
    Obj["instructions"] = std::move(Instructions);

  json::Array AlwaysAttrs;
  for (const std::string &A : ISA.CommonAlwaysAttributes)
    AlwaysAttrs.push_back(A);
  if (!AlwaysAttrs.empty())
    Obj["always_attributes"] = std::move(AlwaysAttrs);

  json::Array AlwaysBlocks;
  for (const AlwaysBlock &B : ISA.AlwaysBlocks)
    AlwaysBlocks.push_back(toJSON(B));
  if (!AlwaysBlocks.empty())
    Obj["always_blocks"] = std::move(AlwaysBlocks);

  return Obj;
}

json::Value toJSON(const InstructionSetDef &Set) {
  json::Array Combines;
  for (const std::string &Name : Set.Combines)
    Combines.push_back(Name);

  json::Object Obj{{"name", Set.Name}, {"isa", toJSON(Set.ISA)}};
  if (!Combines.empty())
    Obj["combines"] = std::move(Combines);
  if (Set.Extends)
    Obj["extends"] = *Set.Extends;
  return Obj;
}

json::Value toJSON(const CoreDef &Core) {
  json::Array Provides;
  for (const std::string &Name : Core.Provides)
    Provides.push_back(Name);

  json::Object Obj{{"name", Core.Name}, {"isa", toJSON(Core.ISA)}};
  if (!Provides.empty())
    Obj["provides"] = std::move(Provides);
  return Obj;
}

} // namespace detail

json::Value targetgen2::toJSON(const Description &D) {
  json::Array Imports;
  for (const std::string &I : D.Imports)
    Imports.push_back(I);

  json::Array InstructionSets;
  for (const InstructionSetDef &S : D.InstructionSets)
    InstructionSets.push_back(::detail::toJSON(S));

  json::Array Cores;
  for (const CoreDef &C : D.Cores)
    Cores.push_back(::detail::toJSON(C));

  return json::Object{{"imports", std::move(Imports)},
                      {"instruction_sets", std::move(InstructionSets)},
                      {"cores", std::move(Cores)}};
}
