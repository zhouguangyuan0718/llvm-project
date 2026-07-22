#ifndef COREDSl_SOURCE_H
#define COREDSl_SOURCE_H

#include "llvm/Support/SMLoc.h"

namespace coredsl {

using SourceLocation = llvm::SMLoc;

struct SourceRange {
  llvm::SMLoc Begin;
  llvm::SMLoc End;
};

} // namespace coredsl

#endif // COREDSl_SOURCE_H
