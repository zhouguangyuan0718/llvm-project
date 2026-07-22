#include "coredsl/Diagnostics.h"

#include "llvm/Support/raw_ostream.h"

namespace coredsl {

void DiagnosticEngine::error(SourceLocation Location,
                             const llvm::Twine &Message) {
  report(llvm::SourceMgr::DK_Error, Location, Message);
}

void DiagnosticEngine::warning(SourceLocation Location,
                               const llvm::Twine &Message) {
  report(llvm::SourceMgr::DK_Warning, Location, Message);
}

void DiagnosticEngine::report(llvm::SourceMgr::DiagKind Kind,
                              SourceLocation Location,
                              const llvm::Twine &Message) {
  if (Kind == llvm::SourceMgr::DK_Error)
    ++ErrorCount;
  Sources.PrintMessage(llvm::errs(), Location, Kind, Message, {}, {}, false);
}

} // namespace coredsl
