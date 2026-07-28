#ifndef COREDSl_TABLEGEN_EMITTER_H
#define COREDSl_TABLEGEN_EMITTER_H

#include "coredsl/AST.h"
#include "llvm/Support/raw_ostream.h"

namespace coredsl {

/// Emits a self-contained LLVM 23 TableGen validation unit for the subset that
/// can already be derived from the frontend IR. This is a bring-up probe, not
/// the final target emitter.
bool emitTableGenSpike(const InstructionSetDecl &InstructionSet,
                       llvm::raw_ostream &OS);

} // namespace coredsl

#endif // COREDSl_TABLEGEN_EMITTER_H
