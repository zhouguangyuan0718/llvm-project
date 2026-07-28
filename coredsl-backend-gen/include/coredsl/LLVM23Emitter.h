#ifndef COREDSL_LLVM23EMITTER_H
#define COREDSL_LLVM23EMITTER_H

#include "coredsl/TargetModel.h"

#include "llvm/ADT/StringRef.h"

namespace llvm {
class raw_ostream;
}

namespace coredsl {

/// Emits a self-contained set of LLVM 23 TableGen source files for the
/// GlobalISel-only scalar subset.  The emitter consumes TargetModel only; it
/// has no dependency on parser or AST implementation details.
void emitLLVM23TableGen(const TargetModel &Model, llvm::raw_ostream &OS);

/// Creates an LLVM experimental-target source directory containing the
/// GlobalISel-only TableGen and C++ skeleton.  The destination must not exist;
/// refusing replacement keeps generation from silently overwriting a target.
bool emitLLVM23Backend(const TargetModel &Model, llvm::StringRef OutputDirectory,
                       llvm::raw_ostream &Errors);

} // namespace coredsl

#endif // COREDSL_LLVM23EMITTER_H
