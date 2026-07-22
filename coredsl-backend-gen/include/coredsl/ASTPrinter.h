#ifndef COREDSl_AST_PRINTER_H
#define COREDSl_AST_PRINTER_H

#include "coredsl/AST.h"

#include <iosfwd>

namespace coredsl {

void printAST(const InstructionSetDecl &Decl, std::ostream &OS);

} // namespace coredsl

#endif // COREDSl_AST_PRINTER_H
