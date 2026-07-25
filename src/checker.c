#include "checker.h"
#include "shl/shl-log.h"

#define CERROR(expr, fmt)                \
  PERROR(STR_FMT":%u:%u: ", fmt,         \
         STR_ARG((expr)->loc.file_path), \
         (expr)->loc.row + 1,            \
         (expr)->loc.col + 1)

#define CERRORF(expr, fmt, ...)          \
  PERROR(STR_FMT":%u:%u: ", fmt,         \
         STR_ARG((expr)->loc.file_path), \
         (expr)->loc.row + 1,            \
         (expr)->loc.col + 1,            \
         __VA_ARGS__)

typedef Da(ExprFunc *) FuncProtos;

typedef struct {
  Type       *return_type;
  Funcs      *funcs;
  Arena      *arena;
  Vars        vars;
  FuncProtos *protos;
} FuncChecker;

ExprFunc *get_func_expr(FuncProtos *protos, Str name) {
  for (u32 i = 0; i < protos->len; ++i) {
    ExprFunc *expr = protos->items[i];
    if (str_eq(expr->name, name))
      return expr;
  }

  return NULL;
}

void free_type_str(Str str, Type *type) {
  if (type->kind == TypeKindFunc)
    free(str.ptr);
}

Str get_type_str(Type *type) {
  switch (type->kind) {
  case TypeKindUnit: return STR_LIT("unit");

  case TypeKindFunc: {
    StringBuilder sb = {0};
    sb_push_char(&sb, '(');
    for (u32 i = 0; i < type->arg_types.len; ++i) {
      if (i > 0)
        sb_push_char(&sb, ' ');
      Str arg_type_str = get_type_str(type->arg_types.items[i]);
      sb_push_str(&sb, arg_type_str);
      free_type_str(arg_type_str, type->arg_types.items[i]);
    }
    sb_push(&sb, ") -> ");
    Str return_type_str = get_type_str(type->return_type);
    sb_push_str(&sb, return_type_str);
    free_type_str(return_type_str, type->return_type);
    return sb_to_str(sb);
  }

  case TypeKindInt:  return STR_LIT("int");
  case TypeKindBool: return STR_LIT("bool");
  case TypeKindStr:  return STR_LIT("str");
  case TypeKindAny:  return STR_LIT("");
  }

  return (Str) {0};
}

bool type_narrow(Expr *expr, Type *a, Type *b, bool log_error) {
  if (a->kind != TypeKindAny) {
    if (!type_eq(a, b)) {
      if (log_error) {
        Str a_str = get_type_str(a);
        Str b_str = get_type_str(b);
        CERRORF(expr, "Trying to assign a value of type "STR_FMT" to a symbol of type "STR_FMT"\n",
                STR_ARG(b_str), STR_ARG(a_str));
        free_type_str(a_str, a);
        free_type_str(b_str, b);
      }
      return false;
    }

    return true;
  }

  if (a->kind == TypeKindAny)
    *a = *b;

  return true;
}

Type *add_func(ExprFunc *expr, Funcs *funcs, Arena *arena);
bool  check_func(u32 index, Funcs *funcs, FuncProtos *protos, Arena *arena);

Type *check_expr(Expr *expr, FuncChecker *checker) {
  switch (expr->kind) {
  case ExprKindStr: {
    CERRORF(expr, "Checking expression of type %u is not implemented yet\n", expr->kind);
    exit(1);
  }

  case ExprKindInt: {
    Type *result = arena_alloc(checker->arena, sizeof(Type));
    result->kind = TypeKindInt;
    return result;
  }

  case ExprKindBool: {
    Type *result = arena_alloc(checker->arena, sizeof(Type));
    result->kind = TypeKindBool;
    return result;
  }

  case ExprKindBlock: {
    CERRORF(expr, "Checking expression of type %u is not implemented yet\n", expr->kind);
    exit(1);
  }

  case ExprKindIdent: {
    u32 index = get_var_index(&checker->vars, checker->vars.len, expr->as.ident.name);
    if (index != (u32) -1)
      return checker->vars.items[index].type;

    u32 func_index = get_func_index(checker->funcs, expr->as.ident.name);
    if (func_index != (u32) -1)
      return checker->funcs->items[func_index].type;

    ExprFunc *func_expr = get_func_expr(checker->protos, expr->as.ident.name);
    if (func_expr)
      return add_func(func_expr, checker->funcs, checker->arena);

    CERRORF(expr, "Symbol "STR_FMT" was not defined before usage\n",
            STR_ARG(expr->as.ident.name));
    return NULL;
  }

  case ExprKindFunc: {
    CERRORF(expr, "Checking expression of type %u is not implemented yet\n", expr->kind);
    exit(1);
  }

  case ExprKindFuncCall: {
    Type *func_type = check_expr(expr->as.func_call.func, checker);

    if (expr->as.func_call.args.len != func_type->arg_types.len) {
      CERRORF(expr, "Invalid arguments count: %u/%u\n",
              expr->as.func_call.args.len,
              func_type->arg_types.len);
      return NULL;
    }

    bool opt = expr->as.func_call.func->kind == ExprKindIdent;
    if (opt)
      opt = get_var_index(&checker->vars, checker->vars.len,
                          expr->as.func_call.func->as.ident.name) == (u32) -1;

    if (!opt) {
      Var var = {
        {},
        func_type,
      };
      DA_APPEND(checker->vars, var);
    }

    bool updated_func = false;
    Exprs *args = &expr->as.func_call.args;
    Types arg_types = {0};

    for (u32 i = 0; i < args->len; ++i) {
      Type *arg_type = check_expr(args->items[i], checker);
      DA_APPEND(arg_types, arg_type);
    }

    for (u32 i = 0; i < args->len; ++i) {
      if (!type_narrow(args->items[i], func_type->arg_types.items[i],
                       arg_types.items[i], false)) {
        Func *func = checker->funcs->items + func_type->func_index;
        func_type = add_func(func->expr, checker->funcs, checker->arena);
        updated_func = true;
        checker->vars.len -= i;
        break;
      }

      Var var = {
        {},
        arg_types.items[i],
      };
      DA_APPEND(checker->vars, var);
    }

    if (updated_func) {
      for (u32 i = 0; i < args->len; ++i) {
        type_narrow(args->items[i], func_type->arg_types.items[i],
                    arg_types.items[i], false);

        Var var = {
          {},
          arg_types.items[i],
        };
        DA_APPEND(checker->vars, var);
      }
    }

    if (arg_types.items)
      free(arg_types.items);

    if (!check_func(func_type->func_index, checker->funcs,
                    checker->protos, checker->arena))
      return NULL;

    return func_type->return_type;
  }

  case ExprKindLet:
  case ExprKindSet:
  case ExprKindRet:
  case ExprKindIf:
  case ExprKindBinOp: {
    CERRORF(expr, "Checking expression of type %u is not implemented yet\n", expr->kind);
    exit(1);
  }
  }

  return NULL;
}

Type *add_func(ExprFunc *expr, Funcs *funcs, Arena *arena) {
  Func new_func = {
    expr,
    arena_alloc(arena, sizeof(Type)),
    expr->args,
    {},
  };
  *new_func.type = (Type) {
    TypeKindFunc,
    {
      .return_type = arena_alloc(arena, sizeof(Type)),
      .arg_types = {},
      .func_index = funcs->len,
    },
  };
  new_func.type->kind = TypeKindFunc;
  *new_func.type->return_type = (Type) { TypeKindAny, {} };
  new_func.type->arg_types.len = expr->args.len;
  new_func.type->arg_types.cap = new_func.type->arg_types.len;
  new_func.type->arg_types.items =
    arena_alloc(arena, new_func.type->arg_types.cap * sizeof(Type *));
  for (u32 i = 0; i < new_func.type->arg_types.len; ++i) {
    new_func.type->arg_types.items[i] = arena_alloc(arena, sizeof(Type));
    *new_func.type->arg_types.items[i] = (Type) { TypeKindAny, {} };
  }
  DA_APPEND(*funcs, new_func);

  return funcs->items[funcs->len - 1].type;
}

bool check_func(u32 index, Funcs *funcs, FuncProtos *protos, Arena *arena) {
  Func *func = funcs->items + index;

  FuncChecker checker = {0};
  checker.return_type = func->type->return_type;
  checker.funcs = funcs;
  checker.arena = arena;
  checker.protos = protos;

  for (u32 i = 0; i < func->expr->args.len; ++i) {
    Var var = {
      func->expr->args.items[i],
      func->type->arg_types.items[i],
    };
    DA_APPEND(checker.vars, var);
  }

  u32 prev_protos_len = protos->len;

  for (u32 i = 0; i + 1 < func->expr->body.len; ++i) {
    if (!check_expr(func->expr->body.items[i], &checker))
      goto fail;
    func = funcs->items + index;
  }

  if (func->expr->body.len > 0) {
    if (func->expr->body.items[func->expr->body.len - 1]->kind != ExprKindRet) {
      Var var = { {}, checker.return_type };
      DA_APPEND(checker.vars, var);
    }

    Type *type = check_expr(func->expr->body.items[func->expr->body.len - 1], &checker);
    func = funcs->items + index;
    if (!type)
      goto fail;
    if (!type_eq(type, checker.return_type)) {
      Str type_str = get_type_str(type);
      Str return_type_str = get_type_str(checker.return_type);
      CERRORF(func->expr->body.items[func->expr->body.len - 1],
              "Cannot return value of type "STR_FMT" from a function returning "STR_FMT"\n",
              STR_ARG(type_str), STR_ARG(return_type_str));
      free_type_str(type_str, type);
      free_type_str(return_type_str, checker.return_type);
      goto fail;
    }

    *checker.return_type = *type;
  }

  func->vars = checker.vars;

  protos->len = prev_protos_len;
  return true;
fail:
  protos->len = prev_protos_len;
  return false;
}

bool check(Exprs *block, Funcs *funcs, Arena *arena) {
  FuncProtos protos = {0};

  for (u32 i = 0; i < block->len; ++i)
    if (block->items[i]->kind == ExprKindFunc)
      DA_APPEND(protos, &block->items[i]->as.func);

  ExprFunc *main_expr = get_func_expr(&protos, STR_LIT("main"));
  if (!main_expr) {
    ERROR("`main` function was not defined\n");
    goto fail;
  }

  if (main_expr->args.len != 0) {
    ERROR("`main` function can only have zero arguments\n");
    goto fail;
  }

  add_func(main_expr, funcs, arena);
  if (!check_func(0, funcs, &protos, arena))
    goto fail;

  if (protos.items)
    free(protos.items);
  return true;
fail:
  if (protos.items)
    free(protos.items);
  return false;
}
