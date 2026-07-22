#include "coredsl/Diagnostics.h"

#include <ostream>
#include <utility>

namespace coredsl {

void DiagnosticEngine::error(const SourceLocation &Location,
                             std::string Message) {
  report(DiagnosticLevel::Error, Location, std::move(Message));
}

void DiagnosticEngine::warning(const SourceLocation &Location,
                               std::string Message) {
  report(DiagnosticLevel::Warning, Location, std::move(Message));
}

void DiagnosticEngine::print(std::ostream &OS) const {
  for (const Diagnostic &Diagnostic : Diagnostics) {
    OS << Diagnostic.Location.File << ':' << Diagnostic.Location.Line << ':'
       << Diagnostic.Location.Column << ": "
       << (Diagnostic.Level == DiagnosticLevel::Error ? "error" : "warning")
       << ": " << Diagnostic.Message << '\n';
  }
}

void DiagnosticEngine::report(DiagnosticLevel Level,
                              const SourceLocation &Location,
                              std::string Message) {
  if (Level == DiagnosticLevel::Error)
    ++ErrorCount;
  Diagnostics.push_back({Level, Location, std::move(Message)});
}

} // namespace coredsl
