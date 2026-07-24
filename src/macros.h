#ifndef MACROS_H
#define MACROS_H

#include "parser.h"

void expand_macros(Expr *expr, Macros *macros,
                   Strs *arg_names, Exprs *args,
                   bool unpack, Arena *arena, Str file_path,
                   i32 row, i32 col, bool is_inlined);
void expand_macros_block(Exprs *block, Macros *macros,
                         Strs *arg_names, Exprs *args,
                         bool unpack, Arena *arena, Str file_path,
                         i32 row, i32 col, bool is_inlined);

#endif // MACROS_H
