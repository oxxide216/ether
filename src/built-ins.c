#include "built-ins.h"

#define TYPE(kind) { kind, {} }

BuiltIn built_ins[] = {
  { STR_LIT("print"), TYPE(TypeKindUnit), 1, { TYPE(TypeKindStr) } },
  { STR_LIT("println"), TYPE(TypeKindUnit), 1, { TYPE(TypeKindStr) } },
  { STR_LIT("len"), TYPE(TypeKindInt), 1, { TYPE(TypeKindList) } },
  // Internal
  { STR_LIT("ether_get_value_len_as_str_2"), TYPE(TypeKindInt), 1, { TYPE(TypeKindInt) } },
  { STR_LIT("ether_get_value_len_as_str_3"), TYPE(TypeKindInt), 1, { TYPE(TypeKindBool) } },
  { STR_LIT("ether_get_value_len_as_str_4"), TYPE(TypeKindInt), 1, { TYPE(TypeKindStr) } },
  { STR_LIT("ether_get_value_len_as_str_4_quoted"), TYPE(TypeKindInt), 1, { TYPE(TypeKindStr) } },
  { STR_LIT("ether_alloc"), TYPE(TypeKindInt), 1, { TYPE(TypeKindInt) } },
  { STR_LIT("ether_value_to_str_2"), TYPE(TypeKindInt), 3, { TYPE(TypeKindInt), TYPE(TypeKindInt), TYPE(TypeKindInt) } },
  { STR_LIT("ether_value_to_str_3"), TYPE(TypeKindInt), 3, { TYPE(TypeKindInt), TYPE(TypeKindInt), TYPE(TypeKindBool) } },
  { STR_LIT("ether_value_to_str_4"), TYPE(TypeKindInt), 3, { TYPE(TypeKindInt), TYPE(TypeKindInt), TYPE(TypeKindStr) } },
  { STR_LIT("ether_value_to_str_4_quoted"), TYPE(TypeKindInt), 3, { TYPE(TypeKindInt), TYPE(TypeKindInt), TYPE(TypeKindStr) } },
  { STR_LIT("ether_rc_inc_4"), TYPE(TypeKindInt), 1, { TYPE(TypeKindStr) } },
  { STR_LIT("ether_rc_inc_5"), TYPE(TypeKindInt), 1, { TYPE(TypeKindList) } },
  { STR_LIT("ether_rc_dec_4"), TYPE(TypeKindInt), 1, { TYPE(TypeKindStr) } },
};

u32 built_ins_len = ARRAY_LEN(built_ins);
