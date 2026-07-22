#ifndef COREDSl_DIAGNOSTICS_H
#define COREDSl_DIAGNOSTICS_H

#include "coredsl/Source.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/SourceMgr.h"

namespace coredsl {

class DiagnosticEngine {
public:
  explicit DiagnosticEngine(llvm::SourceMgr &Sources) : Sources(Sources) {}

  void error(SourceLocation Location, const llvm::Twine &Message);
  void warning(SourceLocation Location, const llvm::Twine &Message);

  bool hasError() const { return ErrorCount != 0; }

private:
  void report(llvm::SourceMgr::DiagKind Kind, SourceLocation Location,
              const llvm::Twine &Message);

  llvm::SourceMgr &Sources;
  unsigned ErrorCount = 0;
};

} // namespace coredsl

#endif // COREDSl_DIAGNOSTICS_H
