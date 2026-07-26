#include "built-ins.h"

#define TYPE(kind) { kind, {} }

BuiltIn built_ins[] = {
  { STR_LIT("print"), TYPE(TypeKindUnit), 1, { TYPE(TypeKindStr) } },
  { STR_LIT("println"), TYPE(TypeKindUnit), 1, { TYPE(TypeKindStr) } },
};

u32 built_ins_len = ARRAY_LEN(built_ins);
