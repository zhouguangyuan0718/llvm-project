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

  json::Object NativeTypes;
  NativeTypes["integer_widths"] = std::move(IntegerWidths);
  NativeTypes["floating_point_types"] = json::Array{};
  Root["native_types"] = std::move(NativeTypes);

  json::Array ScalarArgumentIndices;
  ScalarArgumentIndices.emplace_back(0);

  json::Object Intrinsic;
  Intrinsic["id_cpp"] = "Intrinsic::example_f16_op";
  Intrinsic["scalar_argument_indices"] = std::move(ScalarArgumentIndices);

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
