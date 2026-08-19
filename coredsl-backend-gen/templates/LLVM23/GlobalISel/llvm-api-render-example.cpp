// Example only: construct legalizer template data and render it with LLVM APIs.

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Mustache.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <utility>

using namespace llvm;

static json::Value makeLegalizerInput() {
  json::Object Root;
  Root["target"] = "Example";

  json::Array IntegerWidths;
  IntegerWidths.emplace_back(16);
  IntegerWidths.emplace_back(32);

  json::Array FloatingPointWidths;
  FloatingPointWidths.emplace_back(32);

  json::Object NativeTypes;
  NativeTypes["integer_widths"] = std::move(IntegerWidths);
  NativeTypes["floating_point_widths"] = std::move(FloatingPointWidths);
  Root["native_types"] = std::move(NativeTypes);

  auto MakeMemoryConstraint = [](StringRef Opcode) {
    json::Object ValueTypeIndex;
    ValueTypeIndex["index"] = 0;
    ValueTypeIndex["integer_widths"] = json::Array{16, 32};
    ValueTypeIndex["floating_point_widths"] = json::Array{32};

    json::Array TypeIndices;
    TypeIndices.emplace_back(std::move(ValueTypeIndex));

    json::Object Constraint;
    Constraint["opcode_cpp"] = Opcode;
    Constraint["type_indices"] = std::move(TypeIndices);
    return Constraint;
  };

  json::Array MulIntegerWidths;
  MulIntegerWidths.emplace_back(16);

  json::Object MulTypeIndex;
  MulTypeIndex["index"] = 0;
  MulTypeIndex["integer_widths"] = std::move(MulIntegerWidths);

  json::Array MulTypeIndices;
  MulTypeIndices.emplace_back(std::move(MulTypeIndex));

  json::Object MulConstraint;
  MulConstraint["opcode_cpp"] = "G_MUL";
  MulConstraint["type_indices"] = std::move(MulTypeIndices);

  json::Array FDivFloatingPointWidths;
  FDivFloatingPointWidths.emplace_back(32);

  json::Object FDivTypeIndex;
  FDivTypeIndex["index"] = 0;
  FDivTypeIndex["floating_point_widths"] =
      std::move(FDivFloatingPointWidths);

  json::Array FDivTypeIndices;
  FDivTypeIndices.emplace_back(std::move(FDivTypeIndex));

  json::Object FDivConstraint;
  FDivConstraint["opcode_cpp"] = "G_FDIV";
  FDivConstraint["type_indices"] = std::move(FDivTypeIndices);

  json::Array OpcodeTypeConstraints;
  OpcodeTypeConstraints.emplace_back(MakeMemoryConstraint("G_LOAD"));
  OpcodeTypeConstraints.emplace_back(MakeMemoryConstraint("G_STORE"));
  OpcodeTypeConstraints.emplace_back(std::move(MulConstraint));
  OpcodeTypeConstraints.emplace_back(std::move(FDivConstraint));
  Root["opcode_type_constraints"] = std::move(OpcodeTypeConstraints);

  json::Object ScalarArgumentI16;
  ScalarArgumentI16["integer_width"] = 16;

  json::Object ScalarArgumentI32;
  ScalarArgumentI32["integer_width"] = 32;

  json::Array ScalarArgumentTypes;
  ScalarArgumentTypes.emplace_back(std::move(ScalarArgumentI16));
  ScalarArgumentTypes.emplace_back(std::move(ScalarArgumentI32));

  json::Object ScalarArgument;
  ScalarArgument["index"] = 0;
  ScalarArgument["types"] = std::move(ScalarArgumentTypes);

  json::Array ScalarArguments;
  ScalarArguments.emplace_back(std::move(ScalarArgument));

  json::Object Intrinsic;
  Intrinsic["id_cpp"] = "Intrinsic::example_f16_op";
  Intrinsic["scalar_arguments"] = std::move(ScalarArguments);

  json::Array Intrinsics;
  Intrinsics.emplace_back(std::move(Intrinsic));
  Root["intrinsics"] = std::move(Intrinsics);

  return json::Value(std::move(Root));
}

static Error renderTemplate(StringRef TemplatePath, const json::Value &Data,
                            raw_ostream &OS) {
  auto TemplateOrError = MemoryBuffer::getFile(TemplatePath);
  if (!TemplateOrError)
    return errorCodeToError(TemplateOrError.getError());

  BumpPtrAllocator Allocator;
  StringSaver Saver(Allocator);
  mustache::MustacheContext Context(Allocator, Saver);
  mustache::Template Template((*TemplateOrError)->getBuffer(), Context);

  const json::Object *Root = Data.getAsObject();
  auto Target = Root ? Root->getString("target") : std::nullopt;
  if (!Target)
    return createStringError(inconvertibleErrorCode(),
                             "template data requires a string target");
  Template.registerLambda("target_upper", [Upper = Target->upper()]() {
    return json::Value(Upper);
  });

  // LLVM Mustache defaults to HTML escaping. Generated C++ must be emitted
  // verbatim, including possible '<', '>', '&', quote, and apostrophe tokens.
  Template.overrideEscapeCharacters(mustache::EscapeMap{});
  Template.render(Data, OS);
  return Error::success();
}

int main(int Argc, char **Argv) {
  if (Argc != 2) {
    errs() << "usage: llvm-api-render-example <template.mustache>\n";
    return 1;
  }

  ExitOnError ExitOnErr("llvm-api-render-example: ");
  json::Value Data = makeLegalizerInput();
  ExitOnErr(renderTemplate(Argv[1], Data, outs()));
  return 0;
}
