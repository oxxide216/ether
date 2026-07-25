#include "ast.h"

#define DEST_APPEND(converter, instr)               \
  DA_APPEND(converter->current_func->instrs, instr) \

static Expr *ast_node_clone(Expr *node, Arena *arena) {
  Expr *copy = arena_alloc(arena, sizeof(Expr));
  copy->kind = node->kind;

  switch (node->kind) {
  case ExprKindStr:  break;
  case ExprKindInt:  break;
  case ExprKindBool: break;

  case ExprKindBlock: {
    copy->as.block = ast_clone(&node->as.block, arena);
  } break;

  case ExprKindIdent: break;

  case ExprKindFunc: {
    copy->as.block = ast_clone(&node->as.block, arena);
  } break;

  case ExprKindFuncCall: {
    copy->as.func_call.args = ast_clone(&node->as.func_call.args, arena);
  } break;

  case ExprKindLet: {
    copy->as.let.value = ast_node_clone(node->as.let.value, arena);
  } break;

  case ExprKindSet: {
    copy->as.set.value = ast_node_clone(node->as.set.value, arena);
  } break;

  case ExprKindRet: {
    copy->as.ret.value = ast_node_clone(node->as.ret.value, arena);
  } break;

  case ExprKindIf: {
    copy->as._if.cond = ast_node_clone(node->as._if.cond, arena);
    copy->as._if.if_body = ast_clone(&node->as._if.if_body, arena);
    copy->as._if.else_body = ast_clone(&node->as._if.else_body, arena);
  } break;

  case ExprKindBinOp: {
    copy->as.bin_op.args = ast_clone(&node->as.bin_op.args, arena);
  } break;
  }

  return copy;
}

Exprs ast_clone(Exprs *ast, Arena *arena) {
  Exprs copy;
  copy.len = ast->len;
  copy.items = arena_alloc(arena, copy.len * sizeof(Expr *));

  for (u32 i = 0; i < ast->len; ++i)
    copy.items[i] = ast_node_clone(ast->items[i], arena);

  return copy;
}

Type type_clone(Type *type, Arena *arena) {
  Type new_type = *type;
  if (new_type.kind == TypeKindFunc) {
    new_type.return_type = arena_alloc(arena, sizeof(Type));
    *new_type.return_type = type_clone(type->return_type, arena);
    new_type.arg_types.cap = new_type.arg_types.len;
    new_type.arg_types.items = arena_alloc(arena, new_type.arg_types.cap * sizeof(Type *));
    for (u32 i = 0; i < new_type.arg_types.len; ++i) {
      new_type.arg_types.items[i] = arena_alloc(arena, sizeof(Type));
      *new_type.arg_types.items[i] = type_clone(type->arg_types.items[i], arena);
    }
  }
  return new_type;
}

bool type_eq(Type *a, Type *b) {
  if (a->kind == TypeKindFunc && b->kind == TypeKindFunc) {
    if (a->arg_types.len != b->arg_types.len)
      return false;

    for (u32 i = 0; i < a->arg_types.len; ++i)
      if (!type_eq(a->arg_types.items[i], b->arg_types.items[i]))
        return false;

    return type_eq(a->return_type, b->return_type);
  } else {
    return a->kind == b->kind ||
           a->kind == TypeKindAny ||
           b->kind == TypeKindAny;
  }
}

void type_free(Type *type) {
  if (type->kind == TypeKindFunc) {
    type_free(type->return_type);
    for (u32 i = 0; i < type->arg_types.len; ++i)
      type_free(type->arg_types.items[i]);
  }
}

u32 get_type_size(Type *type) {
  switch (type->kind) {
  case TypeKindUnit: return 0;
  case TypeKindFunc: return 8;
  case TypeKindInt:  return 8;
  case TypeKindBool: return 4;
  case TypeKindStr:  return 16;
  case TypeKindAny:  return 0;
  }

  return 0;
}

u32 get_var_index(Vars *vars, u32 top, Str name) {
  for (u32 i = top <= vars->len ? top : vars->len; i > 0; --i)
    if (str_eq(vars->items[i - 1].name, name))
      return i - 1;

  return (u32) -1;
}

u32 get_func_index(Funcs *funcs, Str name) {
  for (u32 i = 0; i < funcs->len; ++i)
    if (str_eq(funcs->items[i].expr->name, name))
      return i;

  return (u32) -1;
}

u32 get_func_index_with_signature(Funcs *funcs, Str name, Types *arg_types) {
  for (u32 i = 0; i < funcs->len; ++i) {
    Func *func = funcs->items + i;

    if (!str_eq(func->expr->name, name))
      continue;

    if (func->type->arg_types.len != arg_types->len)
      continue;

    bool all = true;

    for (u32 j = 0; j < arg_types->len; ++j) {
      if (!type_eq(func->type->arg_types.items[j], arg_types->items[j])) {
        all = false;
        break;
      }
    }

    if (all)
      return i;
  }

  return (u32) -1;
}
