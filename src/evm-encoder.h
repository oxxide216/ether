#ifndef EVM_ENCODER_H
#define EVM_ENCODER_H

#include "ast.h"

void encode_ast_as_evm_ir(FILE *stream, Funcs *funcs);

#endif // EVM_ENCODER_H
