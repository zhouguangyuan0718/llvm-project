#ifndef COREDSl_DIAGNOSTICS_H
#define COREDSl_DIAGNOSTICS_H

#include "coredsl/Source.h"

#include <iosfwd>
#include <string>
#include <vector>

namespace coredsl {

enum class DiagnosticLevel { Error, Warning };

struct Diagnostic {
  DiagnosticLevel Level;
  SourceLocation Location;
  std::string Message;
};

class DiagnosticEngine {
public:
  void error(const SourceLocation &Location, std::string Message);
  void warning(const SourceLocation &Location, std::string Message);

  bool hasError() const { return ErrorCount != 0; }
  const std::vector<Diagnostic> &diagnostics() const { return Diagnostics; }
  void print(std::ostream &OS) const;

private:
  void report(DiagnosticLevel Level, const SourceLocation &Location,
              std::string Message);

  unsigned ErrorCount = 0;
  std::vector<Diagnostic> Diagnostics;
};

} // namespace coredsl

#endif // COREDSl_DIAGNOSTICS_H
