#ifndef COREDSl_AST_PRINTER_H
#define COREDSl_AST_PRINTER_H

#include "coredsl/AST.h"
#include "llvm/Support/raw_ostream.h"

namespace coredsl {

void printAST(const InstructionSetDecl &Decl, llvm::raw_ostream &OS);

} // namespace coredsl

#endif // COREDSl_AST_PRINTER_H
