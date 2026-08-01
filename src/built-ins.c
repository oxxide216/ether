#include "built-ins.h"

#define TYPE(kind) { kind, {} }

Type *default_get_return_type(BuiltIn *built_in, Types *arg_types, Arena *arena) {
  (void) arg_types;
  (void) arena;

  return &built_in->return_type;
}

Type *prep_get_return_type(BuiltIn *built_in, Types *arg_types, Arena *arena) {
  (void) built_in;

  Type *result = arena_alloc(arena, sizeof(Type));
  result->kind = TypeKindList;
  result->element_type = arg_types->items[0];
  return result;
}

Type *app_get_return_type(BuiltIn *built_in, Types *arg_types, Arena *arena) {
  (void) built_in;

  Type *result = arena_alloc(arena, sizeof(Type));
  result->kind = TypeKindList;
  result->element_type = arg_types->items[1];
  return result;
}

Type **default_get_arg_types(BuiltIn *built_in, Types *arg_types, Arena *arena) {
  (void) arg_types;

  Type **result = arena_alloc(arena, built_in->args_len * sizeof(Type *));
  for (u32 i = 0; i < built_in->args_len; ++i)
    result[i] = built_in->arg_types + i;
  return result;
}

Type **prep_get_arg_types(BuiltIn *built_in, Types *arg_types, Arena *arena) {
  (void) built_in;

  Type **result = arena_alloc(arena, 2 * sizeof(Type *));
  result[0] = arg_types->items[0];
  result[1] = arena_alloc(arena, sizeof(Type));
  result[1]->kind = TypeKindList;
  result[1]->element_type = arg_types->items[0];
  return result;
}

Type **app_get_arg_types(BuiltIn *built_in, Types *arg_types, Arena *arena) {
  (void) built_in;

  Type **result = arena_alloc(arena, 2 * sizeof(Type *));
  result[0] = arena_alloc(arena, sizeof(Type));
  result[0]->kind = TypeKindList;
  result[0]->element_type = arg_types->items[1];
  result[1] = arg_types->items[1];
  return result;
}

BuiltIn built_ins[] = {
  { false, STR_LIT("print"), TYPE(TypeKindUnit), 1, { TYPE(TypeKindStr) }, default_get_return_type, default_get_arg_types },
  { false, STR_LIT("println"), TYPE(TypeKindUnit), 1, { TYPE(TypeKindStr) }, default_get_return_type, default_get_arg_types },
  { false, STR_LIT("len"), TYPE(TypeKindInt), 1, { TYPE(TypeKindList) }, default_get_return_type, default_get_arg_types },
  { false, STR_LIT("prep"), TYPE(TypeKindList), 2, { TYPE(TypeKindAny), TYPE(TypeKindList) }, prep_get_return_type, prep_get_arg_types },
  { false, STR_LIT("app"), TYPE(TypeKindList), 2, { TYPE(TypeKindList), TYPE(TypeKindAny) }, app_get_return_type, app_get_arg_types },
  // Internal
  { true, STR_LIT("ether_get_value_len_as_str_2"), TYPE(TypeKindInt), 1, { TYPE(TypeKindInt) }, NULL, NULL },
  { true, STR_LIT("ether_get_value_len_as_str_3"), TYPE(TypeKindInt), 1, { TYPE(TypeKindBool) }, NULL, NULL },
  { true, STR_LIT("ether_get_value_len_as_str_4"), TYPE(TypeKindInt), 1, { TYPE(TypeKindStr) }, NULL, NULL },
  { true, STR_LIT("ether_get_value_len_as_str_4_quoted"), TYPE(TypeKindInt), 1, { TYPE(TypeKindStr) }, NULL, NULL },
  { true, STR_LIT("ether_alloc"), TYPE(TypeKindInt), 1, { TYPE(TypeKindInt) }, NULL, NULL },
  { true, STR_LIT("ether_free"), TYPE(TypeKindInt), 2, { TYPE(TypeKindInt), TYPE(TypeKindInt) }, NULL, NULL },
  { true, STR_LIT("ether_value_to_str_2"), TYPE(TypeKindInt), 3, { TYPE(TypeKindInt), TYPE(TypeKindInt), TYPE(TypeKindInt) }, NULL, NULL },
  { true, STR_LIT("ether_value_to_str_3"), TYPE(TypeKindInt), 3, { TYPE(TypeKindInt), TYPE(TypeKindInt), TYPE(TypeKindBool) }, NULL, NULL },
  { true, STR_LIT("ether_value_to_str_4"), TYPE(TypeKindInt), 3, { TYPE(TypeKindInt), TYPE(TypeKindInt), TYPE(TypeKindStr) }, NULL, NULL },
  { true, STR_LIT("ether_value_to_str_4_quoted"), TYPE(TypeKindInt), 3, { TYPE(TypeKindInt), TYPE(TypeKindInt), TYPE(TypeKindStr) }, NULL, NULL },
  { true, STR_LIT("ether_rc_inc_1"), TYPE(TypeKindInt), 1, { TYPE(TypeKindFunc) }, NULL, NULL },
  { true, STR_LIT("ether_rc_inc_4"), TYPE(TypeKindInt), 1, { TYPE(TypeKindStr) }, NULL, NULL },
  { true, STR_LIT("ether_rc_inc_5"), TYPE(TypeKindInt), 1, { TYPE(TypeKindList) }, NULL, NULL },
  { true, STR_LIT("ether_rc_dec_4"), TYPE(TypeKindInt), 1, { TYPE(TypeKindStr) }, NULL, NULL },
};

u32 built_ins_len = ARRAY_LEN(built_ins);
