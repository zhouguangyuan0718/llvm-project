#ifndef COREDSl_SOURCE_H
#define COREDSl_SOURCE_H

#include <string>

namespace coredsl {

struct SourceLocation {
  std::string File;
  unsigned Line = 1;
  unsigned Column = 1;
};

struct SourceRange {
  SourceLocation Begin;
  SourceLocation End;
};

} // namespace coredsl

#endif // COREDSl_SOURCE_H
