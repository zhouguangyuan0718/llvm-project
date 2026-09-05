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

  auto IntegerType = [](unsigned Width) {
    json::Object Type;
    Type["integer_width"] = Width;
    return Type;
  };
  auto FloatType = [](unsigned Width) {
    json::Object Type;
    Type["floating_point_width"] = Width;
    return Type;
  };
  auto MakeScalarType = [](unsigned Index, json::Array Types) {
    json::Object ScalarType;
    ScalarType["index"] = Index;
    ScalarType["types"] = std::move(Types);
    return ScalarType;
  };

  auto MakeOperation = [](StringRef IdKey, StringRef Id,
                          json::Array ScalarTypes) {
    json::Object Operation;
    Operation[IdKey] = Id;
    Operation["scalar_types"] = std::move(ScalarTypes);
    return Operation;
  };
  auto MemoryTypes = [&]() {
    json::Array Types;
    Types.emplace_back(IntegerType(16));
    Types.emplace_back(IntegerType(32));
    Types.emplace_back(IntegerType(64));
    Types.emplace_back(FloatType(32));
    return Types;
  };

  json::Array MulTypes;
  MulTypes.emplace_back(IntegerType(16));

  json::Array FDivTypes;
  FDivTypes.emplace_back(FloatType(32));

  json::Array ShlValueTypes;
  ShlValueTypes.emplace_back(IntegerType(32));
  ShlValueTypes.emplace_back(IntegerType(16));

  json::Array ShlAmountTypes;
  ShlAmountTypes.emplace_back(IntegerType(16));

  json::Array IntrinsicTypes;
  IntrinsicTypes.emplace_back(IntegerType(32));
  IntrinsicTypes.emplace_back(IntegerType(16));

  auto SingleScalarType = [&](unsigned Index, json::Array Types) {
    json::Array ScalarTypes;
    ScalarTypes.emplace_back(MakeScalarType(Index, std::move(Types)));
    return ScalarTypes;
  };

  json::Array ShlScalarTypes;
  ShlScalarTypes.emplace_back(MakeScalarType(0, std::move(ShlValueTypes)));
  ShlScalarTypes.emplace_back(MakeScalarType(1, std::move(ShlAmountTypes)));

  json::Array OperationTypeConstraints;
  OperationTypeConstraints.emplace_back(MakeOperation(
      "opcode_cpp", "G_LOAD", SingleScalarType(0, MemoryTypes())));
  OperationTypeConstraints.emplace_back(MakeOperation(
      "opcode_cpp", "G_STORE", SingleScalarType(0, MemoryTypes())));
  OperationTypeConstraints.emplace_back(MakeOperation(
      "opcode_cpp", "G_MUL", SingleScalarType(0, std::move(MulTypes))));
  OperationTypeConstraints.emplace_back(MakeOperation(
      "opcode_cpp", "G_FDIV", SingleScalarType(0, std::move(FDivTypes))));
  OperationTypeConstraints.emplace_back(
      MakeOperation("opcode_cpp", "G_SHL", std::move(ShlScalarTypes)));
  OperationTypeConstraints.emplace_back(
      MakeOperation("intrinsic_id_cpp", "Intrinsic::example_f16_op",
                    SingleScalarType(0, std::move(IntrinsicTypes))));
  Root["operation_type_constraints"] = std::move(OperationTypeConstraints);

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
