#ifndef PARSER_H
#define PARSER_H

#include "arena.h"
#include "ast.h"

typedef struct {
  Str   name;
  Strs  arg_names;
  Exprs body;
  bool  has_unpack;
  u32   row, col;
} Macro;

typedef Da(Macro) Macros;

typedef struct {
  u64    code_hash;
  Exprs  ast;
  Macros macros;
  Strs   included_files;
} CachedAST;

typedef Da(CachedAST) CachedASTs;

Exprs parse_ex(Str code, Str file_path, Macros *macros,
               Strs *included_files, Strs *include_paths,
               CachedASTs *cached_asts, Arena *arena);
Exprs parse(Str code, Str file_path, Strs *include_paths,
            CachedASTs *cached_asts, Macros *macros, Arena *arena);

#endif // PARSER_H
