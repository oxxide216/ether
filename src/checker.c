#include "checker.h"
#include "built-ins.h"
#include "utils.h"
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

BuiltIn *get_built_in(Str name) {
  for (u32 i = 0; i < built_ins_len; ++i)
    if (str_eq(built_ins[i].name, name))
      return built_ins + i;

  return NULL;
}

Type *add_func(ExprFunc *expr, Funcs *funcs, Arena *arena);
bool  check_func(u32 index, Funcs *funcs, FuncProtos *protos, Arena *arena);

Type *check_block(Exprs *block, FuncChecker *checker, bool value_expected);

Type *check_expr(Expr *expr, FuncChecker *checker, bool value_expected) {
  switch (expr->kind) {
  case ExprKindStr: {
    Type *result = arena_alloc(checker->arena, sizeof(Type));
    result->kind = TypeKindStr;
    return result;
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
    u32 prev_protos_len = checker->protos->len;
    Type *result = check_block(&expr->as.block, checker, value_expected);
    checker->protos->len = prev_protos_len;
    return result;
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

    BuiltIn *built_in = get_built_in(expr->as.ident.name);
    if (built_in) {
      Type *result = arena_alloc(checker->arena, sizeof(Type));
      result->kind = TypeKindFunc;
      result->return_type = &built_in->return_type;
      result->arg_types.len = built_in->args_len;
      result->arg_types.cap = result->arg_types.len;
      result->arg_types.items = arena_alloc(checker->arena, result->arg_types.cap * sizeof(Type *));
      for (u32 i = 0; i < result->arg_types.len; ++i)
        result->arg_types.items[i] = built_in->arg_types + i;
      result->built_in = built_in;
      return result;
    }

    CERRORF(expr, "Symbol "STR_FMT" was not defined before usage\n",
            STR_ARG(expr->as.ident.name));
    return NULL;
  }

  case ExprKindFunc: {
    DA_APPEND(*checker->protos, &expr->as.func);

    Type *result = arena_alloc(checker->arena, sizeof(Type));
    result->kind = TypeKindUnit;
    return result;
  }

  case ExprKindFuncCall: {
    bool opt = expr->as.func_call.func->kind == ExprKindIdent;
    if (opt)
      opt = get_var_index(&checker->vars, checker->vars.len,
                          expr->as.func_call.func->as.ident.name) == (u32) -1;

    u32 var_index = checker->vars.len;
    if (!opt) {
      Var var = {
        {},
        NULL,
      };
      DA_APPEND(checker->vars, var);
    }

    Type *func_type = check_expr(expr->as.func_call.func, checker, true);
    if (!func_type)
      return NULL;

    if (!opt)
      checker->vars.items[var_index].type = func_type;

    if (expr->as.func_call.args.len != func_type->arg_types.len) {
      CERRORF(expr, "Invalid arguments count: %u/%u\n",
              expr->as.func_call.args.len,
              func_type->arg_types.len);
      return NULL;
    }

    bool updated_func = false;
    Exprs *args = &expr->as.func_call.args;
    Types arg_types = {0};
    u32 vars_start = checker->vars.len;

    for (u32 i = 0; i < args->len; ++i) {
      Var var = { {}, NULL };
      DA_APPEND(checker->vars, var);

      Type *arg_type = check_expr(args->items[i], checker, true);
      if (!arg_type) {
        if (arg_types.items)
          free(arg_types.items);
        return NULL;
      }

      DA_APPEND(arg_types, arg_type);
    }

    if (func_type->built_in) {
      for (u32 i = 0; i < args->len; ++i) {
        if (!type_eq(arg_types.items[i], func_type->built_in->arg_types + i)) {
          Str a_str = get_type_str(arg_types.items[i]);
          Str b_str = get_type_str(func_type->built_in->arg_types + i);
          CERRORF(args->items[i], "Cannot pass value of type "STR_FMT" as an argument of type "STR_FMT"\n",
                  STR_ARG(a_str), STR_ARG(b_str));
          if (str_eq(func_type->built_in->name, STR_LIT("print")) ||
              str_eq(func_type->built_in->name, STR_LIT("println"))) {
            INFO("If you want to print this value, try using string interpolation instead: f\"{value}\"\n");
          }
          free_type_str(a_str, arg_types.items[i]);
          free_type_str(b_str, func_type->built_in->arg_types + i);
          if (arg_types.items)
            free(arg_types.items);
          return NULL;
        }

        checker->vars.items[vars_start + i].type = arg_types.items[i];
      }

      if (arg_types.items)
        free(arg_types.items);

      expr->as.func_call.built_in = func_type->built_in;
      return func_type->return_type;
    }

    for (u32 i = 0; i < args->len; ++i) {
      if (!type_narrow(args->items[i], func_type->arg_types.items[i],
                       arg_types.items[i], false)) {
        Func *func = checker->funcs->items + func_type->func_index;
        u32 index = get_func_index_with_signature(checker->funcs,
                                                  func->expr->name,
                                                  &arg_types);
        if (index == (u32) -1)
          func_type = add_func(func->expr, checker->funcs, checker->arena);
        else
          func_type = checker->funcs->items[index].type;
        updated_func = true;
        break;
      }

      checker->vars.items[vars_start + i].type = arg_types.items[i];
    }

    if (updated_func) {
      for (u32 i = 0; i < args->len; ++i) {
        type_narrow(args->items[i], func_type->arg_types.items[i],
                    arg_types.items[i], false);
        checker->vars.items[vars_start + i].type = arg_types.items[i];
      }
    }

    if (arg_types.items)
      free(arg_types.items);

    if (!check_func(func_type->func_index, checker->funcs,
                    checker->protos, checker->arena))
      return NULL;

    return func_type->return_type;
  }

  case ExprKindLet: {
    Type *type = check_expr(expr->as.let.value, checker, true);
    if (!type)
      return NULL;

    Var var = {
      expr->as.let.name,
      type,
    };
    DA_APPEND(checker->vars, var);

    return type;
  }

  case ExprKindSet: {
    Type *type = check_expr(expr->as.set.value, checker, true);
    if (!type)
      return NULL;

    u32 var_index = get_var_index(&checker->vars, checker->vars.len, expr->as.set.name);
    if (var_index == (u32) -1) {
      CERRORF(expr, "Symbol "STR_FMT" was not defined before usage\n",
              STR_ARG(expr->as.set.name));
      return NULL;
    }

    Var *var = checker->vars.items + var_index;
    if (!type_narrow(expr, var->type, type, true))
      return NULL;

    return type;
  }

  case ExprKindRet: {
    Type *return_type;
    if (expr->as.ret.value) {
      return_type = check_expr(expr->as.ret.value, checker, true);
      if (!return_type)
        return NULL;
    } else {
      return_type = arena_alloc(checker->arena, sizeof(Type));
      return_type->kind = TypeKindUnit;
    }

    if (!type_narrow(expr->as.ret.value, checker->return_type, return_type, false)) {
      Str a_str = get_type_str(return_type);
      Str b_str = get_type_str(checker->return_type);
      CERRORF(expr->as.ret.value,
              "Cannot return value of type "STR_FMT" from a function returning "STR_FMT"\n",
              STR_ARG(a_str), STR_ARG(b_str));
      free_type_str(a_str, return_type);
      free_type_str(b_str, checker->return_type);
      return NULL;
    }

    Var var = {
      {},
      return_type,
    };
    DA_APPEND(checker->vars, var);

    Type *result = arena_alloc(checker->arena, sizeof(Type));
    result->kind = TypeKindUnit;
    return result;
  } break;

  case ExprKindIf: {
    Type *cond_type = check_expr(expr->as._if.cond, checker, true);
    if (!cond_type)
      return NULL;

    Type type = { TypeKindBool, {} };
    if (!type_eq(cond_type, &type)) {
      Str cond_type_str = get_type_str(cond_type);
      CERRORF(expr->as._if.cond, "Type bool expected in condition, got "STR_FMT"\n",
              STR_ARG(cond_type_str));
      free_type_str(cond_type_str, cond_type);
      return NULL;
    }

    Var var = {
      {},
      cond_type,
    };
    DA_APPEND(checker->vars, var);

    Type *if_type = check_block(&expr->as._if.if_body, checker, value_expected);
    if (!if_type)
      return NULL;

    Type *else_type = check_block(&expr->as._if.else_body, checker, value_expected);
    if (!else_type)
      return NULL;

    if (value_expected && !type_eq(if_type, else_type)) {
      Str if_type_str = get_type_str(if_type);
      Str else_type_str = get_type_str(else_type);
      CERRORF(expr, "Types of then and else branches do not match: got "STR_FMT" and "STR_FMT"\n",
              STR_ARG(if_type_str), STR_ARG(else_type_str));
      free_type_str(if_type_str, if_type);
      free_type_str(else_type_str, else_type);
      return NULL;
    }

    return if_type;
  }

  case ExprKindBinOp: {
    Type type = { TypeKindInt, {} };

    if (expr->as.bin_op.args.len < 2) {
      CERRORF(expr, "Binary operation should have at least 2 arguments, %u were provided\n",
              expr->as.bin_op.args.len);
      return NULL;
    }

    if (expr->as.bin_op.kind >= ErBinOpKindEq && expr->as.bin_op.kind <= ErBinOpKindGe &&
        expr->as.bin_op.args.len != 2) {
      CERRORF(expr, "Comparison operations take exactly 2 arguments, %u were provided\n",
              expr->as.bin_op.args.len);
      return NULL;
    }

    Type *first_arg_type;
    for (u32 i = 0; i < expr->as.bin_op.args.len; ++i) {
      Type *arg_type = check_expr(expr->as.bin_op.args.items[i], checker, true);
      if (!arg_type)
        return NULL;

      if (i == 0) {
        first_arg_type = arg_type;
        if (!type_narrow(expr->as.bin_op.args.items[i], arg_type, &type, true))
          return NULL;

        if (expr->as.bin_op.args.len > 2) {
          Var var = {
            {},
            first_arg_type,
          };
          DA_APPEND(checker->vars, var);
        }
      } else {
        if (!type_narrow(expr->as.bin_op.args.items[i], arg_type, first_arg_type, true))
          return NULL;
      }

      Var var = {
        {},
        arg_type,
      };
      DA_APPEND(checker->vars, var);
    }

    if (expr->as.bin_op.kind >= ErBinOpKindEq && expr->as.bin_op.kind <= ErBinOpKindGe) {
      Type *result = arena_alloc(checker->arena, sizeof(Type));
      result->kind = TypeKindBool;
      return result;
    }

    return first_arg_type;
  }

  case ExprKindFStr: {
    Type *type = arena_alloc(checker->arena, sizeof(Type));
    type->kind = TypeKindInt;

    Var var = { {}, type };

    DA_APPEND(checker->vars, var);
    DA_APPEND(checker->vars, var);

    for (u32 i = 0; i < expr->as.fstr.parts.len; ++i) {
      FStrPart *part = expr->as.fstr.parts.items + i;
      if (!part->is_var)
        continue;

      u32 index = get_var_index(&checker->vars, checker->vars.len, part->str);
      if (index == (u32) -1) {
        CERRORF(expr, "Variable "STR_FMT" was not defined before usage\n",
                STR_ARG(part->str));
        return NULL;
      }

      if (checker->vars.items[index].type->kind == TypeKindFunc) {
        Str type_str = get_type_str(checker->vars.items[index].type);
        CERRORF(expr, "Cannot format variable "STR_FMT" of type "STR_FMT"\n",
                STR_ARG(part->str), STR_ARG(type_str));
        free_type_str(type_str, checker->vars.items[index].type);
        return NULL;
      }
    }

    DA_APPEND(checker->vars, var);

    Type *result = arena_alloc(checker->arena, sizeof(Type));
    result->kind = TypeKindStr;
    return result;
  }
  }

  return NULL;
}

Type *check_block(Exprs *block, FuncChecker *checker, bool value_expected) {
  for (u32 i = 0; i + 1 < block->len; ++i)
    check_expr(block->items[i], checker, false);

  if (block->len > 0)
    return check_expr(block->items[block->len - 1], checker, value_expected);

  Type *result = arena_alloc(checker->arena, sizeof(Type));
  result->kind = TypeKindUnit;
  return result;
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
    if (!check_expr(func->expr->body.items[i], &checker, false))
      goto fail;
    func = funcs->items + index;
  }

  if (func->expr->body.len > 0) {
    bool is_last_ret = func->expr->body.items[func->expr->body.len - 1]->kind == ExprKindRet;

    if (!is_last_ret && !str_eq(func->expr->name, STR_LIT("main"))) {
      Var var = { {}, checker.return_type };
      DA_APPEND(checker.vars, var);
    }

    Type *type = check_expr(func->expr->body.items[func->expr->body.len - 1],
                            &checker, true);
    func = funcs->items + index;
    if (!type)
      goto fail;

    if (!is_last_ret) {
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

  Type *main_return_type = funcs->items[0].type->return_type;
  if (main_return_type->kind != TypeKindInt) {
    if (main_return_type->kind == TypeKindUnit) {
      Expr *expr0 = arena_alloc(arena, sizeof(Expr));
      expr0->kind = ExprKindInt;
      expr0->as._int._int = 0;

      Expr *expr1 = arena_alloc(arena, sizeof(Expr));
      expr1->kind = ExprKindRet;
      expr1->as.ret.value = expr0;

      DA_ARENA_APPEND(main_expr->body, expr1, arena);
    } else {
      Str main_return_type_str = get_type_str(main_return_type);
      ERROR("Unexpected `main` function return type: "STR_FMT", expected int\n",
            STR_ARG(main_return_type_str));
      free_type_str(main_return_type_str, main_return_type);
      goto fail;
    }
  }

  if (protos.items)
    free(protos.items);
  return true;
fail:
  if (protos.items)
    free(protos.items);
  return false;
}
