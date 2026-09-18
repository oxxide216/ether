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

typedef struct {
  u32    index;
  Type  *return_type;
  Funcs *funcs;
  Arena *arena;
  Vars   vars;
  Vars  *captured_vars;
  Vars   used_captured_vars;
  u32   *lambda_counter;
} FuncChecker;

void free_type_str(Str str, Type *type) {
  if (type->kind == TypeKindFunc)
    free(str.ptr);
}

Str get_type_str(Type *type) {
  switch (type->kind) {
  case TypeKindUnit: return STR_LIT("unit");

  case TypeKindFunc: {
    StringBuilder sb = {0};
    sb_push(&sb, "(fun ");
    Str return_type_str = get_type_str(type->return_type);
    sb_push_str(&sb, return_type_str);
    free_type_str(return_type_str, type->return_type);
    sb_push(&sb, " (");
    for (u32 i = 0; i < type->arg_types.len; ++i) {
      if (i > 0)
        sb_push_char(&sb, ' ');
      Str arg_type_str = get_type_str(type->arg_types.items[i]);
      sb_push_str(&sb, arg_type_str);
      free_type_str(arg_type_str, type->arg_types.items[i]);
    }
    sb_push(&sb, "))");
    return sb_to_str(sb);
  }

  case TypeKindInt:  return STR_LIT("int");
  case TypeKindBool: return STR_LIT("bool");
  case TypeKindStr:  return STR_LIT("str");

  case TypeKindList: {
    StringBuilder sb = {0};
    sb_push_char(&sb, '[');
    Str type_str = get_type_str(type->element_type);
    sb_push_str(&sb, type_str);
    free_type_str(type_str, type->element_type);
    sb_push_char(&sb, ']');
    return sb_to_str(sb);
  }

  case TypeKindAny: return STR_LIT("any");
  }

  return (Str) {0};
}

bool type_narrow(Expr *expr, Type *a, Type *b, bool log_error) {
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

  if (a->kind == TypeKindAny) {
    *a = *b;
  } else if (b->kind == TypeKindAny) {
    *b = *a;
  } else if (a->kind == TypeKindFunc) {
    if (!type_narrow(expr, a->return_type, b->return_type, log_error))
      return false;

    if (a->arg_types.len != b->arg_types.len) {
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

    for (u32 i = 0; i < a->arg_types.len; ++i)
      if (!type_narrow(expr, a->arg_types.items[i], b->arg_types.items[i], log_error))
        return false;
  } else if (a->kind == TypeKindList) {
    return type_narrow(expr, a->element_type, b->element_type, log_error);
  }

  return true;
}

Func *get_func(Funcs *funcs, Str name) {
  for (u32 i = 0; i < funcs->len; ++i)
    if (str_eq(funcs->items[i].expr->name, name))
      return funcs->items + i;

  return NULL;
}

BuiltIn *get_built_in(Str name) {
  for (u32 i = 0; i < built_ins_len; ++i)
    if (!built_ins[i].is_internal && str_eq(built_ins[i].name, name))
      return built_ins + i;

  return NULL;
}

void make_func_expr_name_lambda_name(ExprFunc *expr, Arena *arena, u32 *lambda_counter) {
  StringBuilder sb = {0};
  sb_push(&sb, "lambda_");
  sb_push_u32(&sb, (*lambda_counter)++);

  expr->name.len = sb.len;
  expr->name.ptr = arena_alloc(arena, expr->name.len);
  memcpy(expr->name.ptr, sb.buffer, expr->name.len);
  free(sb.buffer);
}

Type *add_func(ExprFunc *expr, bool is_lambda, Funcs *funcs, Arena *arena);
bool  check_func(u32 index, Funcs *funcs, Arena *arena, u32 *lambda_counter);

Type *check_block(Exprs *block, FuncChecker *checker, bool value_expected);

Type *check_expr(Expr *expr, FuncChecker *checker, bool value_expected) {
  switch (expr->kind) {
  case ExprKindStr: {
    expr->type = arena_alloc(checker->arena, sizeof(Type));
    expr->type->kind = TypeKindStr;
    return expr->type;
  }

  case ExprKindInt: {
    expr->type = arena_alloc(checker->arena, sizeof(Type));
    expr->type->kind = TypeKindInt;
    return expr->type;
  }

  case ExprKindBool: {
    expr->type = arena_alloc(checker->arena, sizeof(Type));
    expr->type->kind = TypeKindBool;
    return expr->type;
  }

  case ExprKindBlock: {
    expr->type = check_block(&expr->as.block, checker, value_expected);
    return expr->type;
  }

  case ExprKindIdent: {
    u32 index = get_var_index(&checker->vars, checker->vars.len, expr->as.ident.name);
    if (index != (u32) -1) {
      expr->type = checker->vars.items[index].type;
      return expr->type;
    }

    if (checker->captured_vars) {
      index = get_var_index(checker->captured_vars, checker->vars.len, expr->as.ident.name);
      if (index != (u32) -1) {
        u32 used_index = get_var_index(&checker->used_captured_vars,
                                       checker->used_captured_vars.len,
                                       expr->as.ident.name);
        if (used_index == (u32) -1)
          DA_ARENA_APPEND(checker->used_captured_vars,
                          checker->captured_vars->items[index],
                          checker->arena);
        expr->type = checker->captured_vars->items[index].type;
        return expr->type;
      }
    }

    u32 func_index = get_func_index(checker->funcs, expr->as.ident.name);
    if (func_index != (u32) -1) {
      expr->type = checker->funcs->items[func_index].type;
      return expr->type;
    }

    BuiltIn *built_in = get_built_in(expr->as.ident.name);
    if (built_in) {
      expr->type = arena_alloc(checker->arena, sizeof(Type));
      expr->type->kind = TypeKindFunc;
      expr->type->arg_types.len = built_in->args_len;
      expr->type->arg_types.cap = expr->type->arg_types.len;
      expr->type->built_in = built_in;
      return expr->type;
    }

    CERRORF(expr, "Variable or function "STR_FMT" was not defined before usage\n",
            STR_ARG(expr->as.ident.name));
    return NULL;
  }

  case ExprKindFunc: {
    bool is_lambda = expr->as.func.name.len == 0;
    if (is_lambda)
      make_func_expr_name_lambda_name(&expr->as.func, checker->arena, checker->lambda_counter);

    expr->type = add_func(&expr->as.func, is_lambda, checker->funcs, checker->arena);
    Func *func = checker->funcs->items + checker->funcs->len - 1;
    checker->captured_vars = &checker->funcs->items[checker->index].captured_vars;

    for (u32 i = 0; i < checker->vars.len; ++i)
      if (checker->vars.items[i].name.len > 0)
        ++func->captured_vars.len;

    func->captured_vars.cap = func->captured_vars.len;
    func->captured_vars.items =
      arena_alloc(checker->arena, func->captured_vars.cap * sizeof(Var));

    u32 index = 0;
    for (u32 i = 0; i < checker->vars.len; ++i) {
      if (checker->vars.items[i].name.len > 0) {
        func->captured_vars.items[index] = checker->vars.items[i];
        ++index;
      }
    }

    if (!check_func(expr->type->func_index, checker->funcs,
                    checker->arena, checker->lambda_counter))
      return NULL;

    return expr->type;
  }

  case ExprKindFuncCall: {
    bool opt = expr->as.func_call.func->kind == ExprKindIdent;
    if (opt)
      opt = get_var_index(&checker->vars, checker->vars.len,
                          expr->as.func_call.func->as.ident.name) == (u32) -1;

    Type *func_type = check_expr(expr->as.func_call.func, checker, true);
    if (!func_type)
      return NULL;

    if (expr->as.func_call.args.len != func_type->arg_types.len) {
      CERRORF(expr, "Invalid arguments count: %u/%u\n",
              expr->as.func_call.args.len,
              func_type->arg_types.len);
      return NULL;
    }

    bool updated_func = false;
    Exprs *args = &expr->as.func_call.args;
    Types arg_types = {0};

    for (u32 i = 0; i < args->len; ++i) {
      Type *arg_type = check_expr(args->items[i], checker, true);
      if (!arg_type) {
        if (arg_types.items)
          free(arg_types.items);
        return NULL;
      }

      DA_APPEND(arg_types, arg_type);
    }

    if (func_type->built_in) {
      func_type->return_type =
        func_type->built_in->get_return_type(func_type->built_in, &arg_types, checker->arena);
      func_type->arg_types.items =
        func_type->built_in->get_arg_types(func_type->built_in, &arg_types, checker->arena);

      for (u32 i = 0; i < args->len; ++i) {
        if (!type_narrow(args->items[i], arg_types.items[i],
                         func_type->arg_types.items[i], false)) {
          Str a_str = get_type_str(arg_types.items[i]);
          Str b_str = get_type_str(func_type->arg_types.items[i]);
          CERRORF(args->items[i], "Cannot pass value of type "STR_FMT" as an argument of type "STR_FMT"\n",
                  STR_ARG(a_str), STR_ARG(b_str));
          if (str_eq(func_type->built_in->name, STR_LIT("print")) ||
              str_eq(func_type->built_in->name, STR_LIT("println"))) {
            INFO("If you want to print this value, try using string interpolation instead: i\"{value}\"\n");
          }
          free_type_str(a_str, arg_types.items[i]);
          free_type_str(b_str, func_type->arg_types.items[i]);
          free(arg_types.items);
          return NULL;
        }
      }

      if (arg_types.items)
        free(arg_types.items);

      expr->as.func_call.built_in = func_type->built_in;
      expr->type = func_type->return_type;
      return expr->type;
    }

    for (u32 i = 0; i < args->len; ++i) {
      if (!type_narrow(args->items[i], func_type->arg_types.items[i],
                       arg_types.items[i], false)) {
        Func *func = checker->funcs->items + func_type->func_index;
        u32 index = get_func_index_with_signature(checker->funcs,
                                                  func->expr->name,
                                                  &arg_types);
        if (index == (u32) -1)
          func_type = add_func(func->expr, func->is_lambda,
                               checker->funcs, checker->arena);
        else
          func_type = checker->funcs->items[index].type;
        updated_func = true;
        break;
      }
    }

    if (updated_func)
      for (u32 i = 0; i < args->len; ++i)
        type_narrow(args->items[i], func_type->arg_types.items[i],
                    arg_types.items[i], false);

    if (arg_types.items)
      free(arg_types.items);

    if (!check_func(func_type->func_index, checker->funcs,
                    checker->arena, checker->lambda_counter))
      return NULL;

    expr->type = func_type->return_type;
    return expr->type;
  }

  case ExprKindLet: {
    u32 var_index = checker->vars.len;
    Var var = { {}, NULL };
    DA_APPEND(checker->vars, var);

    expr->type = check_expr(expr->as.let.value, checker, true);
    if (!expr->type)
      return NULL;

    checker->vars.items[var_index].name = expr->as.let.name;
    checker->vars.items[var_index].type = expr->type;

    return expr->type;
  }

  case ExprKindSet: {
    u32 var_index = get_var_index(&checker->vars, checker->vars.len, expr->as.set.name);
    Var *var = NULL;
    if (var_index == (u32) -1) {
      if (checker->captured_vars) {
        var_index = get_var_index(checker->captured_vars, checker->vars.len, expr->as.set.name);
        if (var_index != (u32) -1) {
          u32 used_index = get_var_index(&checker->used_captured_vars,
                                         checker->used_captured_vars.len,
                                         expr->as.set.name);
          if (used_index == (u32) -1)
            DA_ARENA_APPEND(checker->used_captured_vars,
                            checker->captured_vars->items[var_index],
                            checker->arena);
          var = checker->captured_vars->items + var_index;
        }
      }

      if (var_index == (u32) -1) {
        CERRORF(expr, "Variable "STR_FMT" was not defined before usage\n",
                STR_ARG(expr->as.set.name));
        return NULL;
      }
    }

    Func *func = checker->funcs->items + checker->index;
    if (var_index < func->expr->args.len) {
      CERROR(expr, "Function parameters are immutable\n");
      return NULL;
    }

    if (!var)
      var = checker->vars.items + var_index;

    expr->type = check_expr(expr->as.set.value, checker, true);
    if (!expr->type)
      return NULL;

    if (!type_narrow(expr, var->type, expr->type, true))
      return NULL;

    return expr->type;
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

    if (!type_narrow(expr->as.ret.value, checker->return_type,
                     return_type, false)) {
      Str a_str = get_type_str(return_type);
      Str b_str = get_type_str(checker->return_type);
      CERRORF(expr->as.ret.value,
              "Cannot return value of type "STR_FMT" from a function returning "STR_FMT"\n",
              STR_ARG(a_str), STR_ARG(b_str));
      free_type_str(a_str, return_type);
      free_type_str(b_str, checker->return_type);
      return NULL;
    }

    expr->type = arena_alloc(checker->arena, sizeof(Type));
    expr->type->kind = TypeKindUnit;
    return expr->type;
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

    expr->type = check_block(&expr->as._if.if_body, checker, value_expected);
    if (!expr->type)
      return NULL;

    Type *else_type = check_block(&expr->as._if.else_body, checker, value_expected);
    if (!else_type)
      return NULL;

    if (value_expected && !type_eq(expr->type, else_type)) {
      Str if_type_str = get_type_str(expr->type);
      Str else_type_str = get_type_str(else_type);
      CERRORF(expr, "Types of then and else branches do not match: got "STR_FMT" and "STR_FMT"\n",
              STR_ARG(if_type_str), STR_ARG(else_type_str));
      free_type_str(if_type_str, expr->type);
      free_type_str(else_type_str, else_type);
      return NULL;
    }

    return expr->type;
  }

  case ExprKindBinOp: {
    if (expr->as.bin_op.args.len < 2) {
      CERRORF(expr, "Binary operation should have at least 2 arguments, %u were provided\n",
              expr->as.bin_op.args.len);
      return NULL;
    }

    bool is_cmp =
      expr->as.bin_op.kind >= ErBinOpKindEq &&
      expr->as.bin_op.kind <= ErBinOpKindGe;

    Type *default_arg_type = arena_alloc(checker->arena, sizeof(Type));
    *default_arg_type = (Type) { TypeKindInt, {} };

    if (is_cmp) {
      expr->type = arena_alloc(checker->arena, sizeof(Type));
      *expr->type = (Type) { TypeKindBool, {} };
    } else {
      expr->type = default_arg_type;
    }

    for (u32 i = 0; i < expr->as.bin_op.args.len; ++i) {
      Type *arg_type = check_expr(expr->as.bin_op.args.items[i], checker, true);
      if (!arg_type)
        return NULL;

      if (i == 0) {
        if (!type_narrow(expr->as.bin_op.args.items[i], arg_type,
                         default_arg_type, true))
          return NULL;
      } else {
        if (!type_narrow(expr->as.bin_op.args.items[i], arg_type,
                         default_arg_type, true))
          return NULL;
      }
    }

    return expr->type;
  }

  case ExprKindFStr: {
    for (u32 i = 0; i < expr->as.fstr.parts.len; ++i) {
      FStrPart *part = expr->as.fstr.parts.items + i;
      if (!part->expr)
        continue;

      Type *part_type = check_expr(part->expr, checker, true);
      if (!part_type)
        return NULL;

      if (part_type->kind == TypeKindFunc) {
        Str type_str = get_type_str(part_type);
        CERRORF(expr, "Cannot format value of type "STR_FMT"\n",
                STR_ARG(type_str));
        free_type_str(type_str, part_type);
        return NULL;
      }

      if (part_type->kind == TypeKindUnit) {
        part->str = STR_LIT("unit");
        part->expr = NULL;

        if (i > 0 && !part[-1].expr) {
          Str new_str;
          new_str.len = part[-1].str.len + part->str.len;
          new_str.ptr = arena_alloc(checker->arena, new_str.len);
          memcpy(new_str.ptr, part[-1].str.ptr, part[-1].str.len);
          memcpy(new_str.ptr + part[-1].str.len, part->str.ptr, part->str.len);
          part[-1].str = new_str;
          DA_REMOVE_AT(expr->as.fstr.parts, i);
          --i;
        } else if (i + 1 < expr->as.fstr.parts.len && !part[1].expr) {
          Str new_str;
          new_str.len = part[1].str.len + part->str.len;
          new_str.ptr = arena_alloc(checker->arena, new_str.len);
          memcpy(new_str.ptr, part[1].str.ptr, part[1].str.len);
          memcpy(new_str.ptr + part[1].str.len, part->str.ptr, part->str.len);
          part[1].str = new_str;
          DA_REMOVE_AT(expr->as.fstr.parts, i);
          --i;
        }
      }
    }

    expr->type = arena_alloc(checker->arena, sizeof(Type));
    expr->type->kind = TypeKindStr;
    return expr->type;
  }

  case ExprKindList: {
    Type *type = arena_alloc(checker->arena, sizeof(Type));
    type->kind = TypeKindInt;

    expr->type = arena_alloc(checker->arena, sizeof(Type));
    expr->type->kind = TypeKindList;

    if (expr->as.list.elements.len == 0) {
      expr->type->element_type = arena_alloc(checker->arena, sizeof(Type));
      expr->type->element_type->kind = TypeKindAny;
    } else {
      Type *element_type = check_expr(expr->as.list.elements.items[0], checker, true);
      expr->type->element_type = element_type;
      if (expr->type->element_type->kind == TypeKindUnit) {
        CERROR(expr->as.list.elements.items[0], "List cannot hold type unit\n");
        return NULL;
      }
    }

    for (u32 i = 1; i < expr->as.list.elements.len; ++i) {
      Expr *element = expr->as.list.elements.items[i];
      Type *element_type = check_expr(element, checker, true);
      if (!type_eq(element_type, expr->type->element_type)) {
        Str a_str = get_type_str(element_type);
        Str b_str = get_type_str(expr->type->element_type);
        CERRORF(element, "Cannot add list element of type "STR_FMT" to a list with element type "STR_FMT"\n",
                STR_ARG(a_str), STR_ARG(b_str));
        free_type_str(a_str, element_type);
        free_type_str(b_str, expr->type->element_type);
        return NULL;
      }
    }

    return expr->type;
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

Type *add_func(ExprFunc *expr, bool is_lambda, Funcs *funcs, Arena *arena) {
  Func new_func = {
    expr,
    arena_alloc(arena, sizeof(Type)),
    expr->args,
    {},
    {},
    is_lambda,
    false,
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

bool check_func(u32 index, Funcs *funcs, Arena *arena, u32 *lambda_counter) {
  Func *func = funcs->items + index;

  if (func->is_checked)
    return true;
  func->is_checked = true;

  FuncChecker checker = {0};
  checker.index = index;
  checker.return_type = func->type->return_type;
  checker.funcs = funcs;
  checker.arena = arena;
  checker.captured_vars = &func->captured_vars;
  checker.lambda_counter = lambda_counter;

  for (u32 i = 0; i < func->expr->args.len; ++i) {
    Var var = {
      func->expr->args.items[i],
      func->type->arg_types.items[i],
    };
    DA_APPEND(checker.vars, var);
  }

  for (u32 i = 0; i + 1 < func->expr->body.len; ++i) {
    if (!check_expr(func->expr->body.items[i], &checker, false))
      return false;

    func = funcs->items + index;
  }

  if (func->expr->body.len > 0) {
    bool is_last_ret = func->expr->body.items[func->expr->body.len - 1]->kind == ExprKindRet;
    bool is_main = str_eq(func->expr->name, STR_LIT("main"));
    bool inserted_var = !is_last_ret && !is_main;

    u32 var_index = checker.vars.len;
    if (inserted_var) {
      Var var = { {}, checker.return_type };
      DA_APPEND(checker.vars, var);
    }

    Type *type = check_expr(func->expr->body.items[func->expr->body.len - 1],
                            &checker, !is_main);
    func = funcs->items + index;
    if (!type)
      return false;

    if (!is_last_ret) {
      if (!type_narrow(NULL, type, checker.return_type, false)) {
        Str type_str = get_type_str(type);
        Str return_type_str = get_type_str(checker.return_type);
        CERRORF(func->expr->body.items[func->expr->body.len - 1],
                "Cannot return value of type "STR_FMT" from a function returning "STR_FMT"\n",
                STR_ARG(type_str), STR_ARG(return_type_str));
        free_type_str(type_str, type);
        free_type_str(return_type_str, checker.return_type);
        return false;
      }
    }

    if (inserted_var && get_type_size(func->type->return_type) == 0)
      DA_REMOVE_AT(checker.vars, var_index);
  }

  func->vars = checker.vars;
  func->captured_vars = checker.used_captured_vars;

  for (u32 i = func->captured_vars.len; i > 0; --i)
    DA_INSERT(func->vars, 0, func->captured_vars.items[i - 1]);

  return true;
}

bool check(Exprs *block, Funcs *funcs, Arena *arena) {
  ExprFunc *main_expr = NULL;
  u32 main_func_index = 0;

  for (u32 i = 0; i < block->len; ++i) {
    if (block->items[i]->kind == ExprKindFunc) {
      if (str_eq(block->items[i]->as.func.name, STR_LIT("main"))) {
        main_expr = &block->items[i]->as.func;
        main_func_index = funcs->len;
      }
      add_func(&block->items[i]->as.func, false, funcs, arena);
    }
  }

  if (!main_expr) {
    ERROR("`main` function was not defined\n");
    INFO("Try defining it like this:\n");
    printf("  (fun main()\n");
    printf("    (println \"Hello, World!\"))\n");
    return false;
  }

  if (main_expr->args.len != 0) {
    ERROR("`main` function can only have zero arguments\n");
    return false;
  }

  u32 lambda_counter = 0;
  if (!check_func(main_func_index, funcs, arena, &lambda_counter))
    return false;

  for (u32 i = 0; i < funcs->len; ++i) {
    Func *func = funcs->items + i;
    for (u32 j = 0; j < func->vars.len; ++j) {
      Var *var = func->vars.items + j;
      if (var->type->kind == TypeKindList &&
          var->type->element_type->kind == TypeKindAny)
        var->type->element_type->kind = TypeKindInt;
    }
  }

  Func *main_func = funcs->items + main_func_index;

  Var var = {
    {},
    arena_alloc(arena, sizeof(Type)),
  };
  var.type->kind = TypeKindInt;
  DA_APPEND(main_func->vars, var);

  Expr *value_expr = arena_alloc(arena, sizeof(Expr));
  value_expr->kind = ExprKindInt;
  value_expr->as._int._int = 0;
  value_expr->type = var.type;

  Expr *ret_expr = arena_alloc(arena, sizeof(Expr));
  ret_expr->kind = ExprKindRet;
  ret_expr->as.ret.value = value_expr;

  DA_ARENA_APPEND(main_func->expr->body, ret_expr, arena);

  return true;
}
