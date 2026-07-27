#ifndef BUILT_INS_H
#define BUILT_INS_H

#include "ast.h"

#define BUILT_INS_ARGS_MAX 10

struct BuiltIn {
  bool is_internal;
  Str  name;
  Type return_type;
  u32  args_len;
  Type arg_types[BUILT_INS_ARGS_MAX];
};

extern BuiltIn built_ins[];
extern u32 built_ins_len;

#endif // BUILT_INS_H
