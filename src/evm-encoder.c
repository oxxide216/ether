#include "evm-encoder.h"
#include "evm/ir.h"
#include "built-ins.h"
#include "shl/shl-log.h"

typedef Da(Var *) VarRefs;

typedef struct {
  Str      name;
  Var     *_return;
  VarRefs  args;
  Vars     vars;
} BuiltInToGen;

typedef Da(BuiltInToGen) BuiltInsToGen;

typedef struct {
  FILE          *stream;
  Arena         *arena;
  Funcs         *funcs;
  Instrs         instrs;
  Data           data;
  Vars          *vars;
  u32            vars_defined;
  BuiltInsToGen  built_ins_to_gen;
} Encoder;

static void encode_str(FILE *stream, Str str) {
  fwrite(&str.len, sizeof(str.len), 1, stream);
  fwrite(str.ptr, 1, str.len, stream);
}

static ValueKind get_type_value_kind(Type *type) {
  switch (type->kind) {
  case TypeKindUnit: return ValueKindUnsigned;
  case TypeKindFunc: return ValueKindUnsigned;
  case TypeKindInt:  return ValueKindSigned;
  case TypeKindBool: return ValueKindUnsigned;
  case TypeKindStr:  return ValueKindUnsigned;
  case TypeKindList: return ValueKindUnsigned;
  case TypeKindAny:  return 0;
  }

  return 0;
}

static void encode_type(FILE *stream, Type *type) {
  (void) type;

  u32 size = get_type_size(type);
  u8 kind = get_type_value_kind(type);
  fwrite(&size, sizeof(size), 1, stream);
  fwrite(&kind, sizeof(kind), 1, stream);
}

static u32 define_var(Encoder *encoder) {
  Segments segments;
  segments.len = 1;
  segments.cap = segments.len;
  segments.items = arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));
  segments.items[0].offset = 0;
  segments.items[0].size = get_type_size(encoder->vars->items[encoder->vars_defined].type);

  Instr instr = {
    InstrKindAlloc,
    {
      .alloc = {
        encoder->vars_defined,
        segments,
      },
    },
  };
  DA_APPEND(encoder->instrs, instr);

  return encoder->vars_defined++;
}

static BinOpKind bin_op_kind_to_evm(ErBinOpKind kind) {
  switch (kind) {
  case ErBinOpKindAdd: return BinOpKindAddInt;
  case ErBinOpKindSub: return BinOpKindSubInt;
  case ErBinOpKindMul: return BinOpKindMulInt;
  case ErBinOpKindDiv: return BinOpKindDivInt;
  case ErBinOpKindRem: return BinOpKindRem;
  case ErBinOpKindEq:  return BinOpKindEqInt;
  case ErBinOpKindNe:  return BinOpKindNeInt;
  case ErBinOpKindLs:  return BinOpKindLsInt;
  case ErBinOpKindLe:  return BinOpKindLeInt;
  case ErBinOpKindGt:  return BinOpKindGtInt;
  case ErBinOpKindGe:  return BinOpKindGeInt;
  }

  return 0;
}

static void sb_push_type_hash(StringBuilder *sb, Funcs *funcs, Type *type) {
  sb_push_u32(sb, type->kind);
  if (type->kind == TypeKindFunc) {
    if (funcs) {
      sb_push_str(sb, funcs->items[type->func_index].expr->name);
    } else {
      sb_push_type_hash(sb, NULL, type->return_type);
      sb_push_u32(sb, type->arg_types.len);
      for (u32 i = 0; i < type->arg_types.len; ++i)
        sb_push_type_hash(sb, NULL, type->arg_types.items[i]);
    }
  } else if (type->kind == TypeKindList) {
    sb_push_type_hash(sb, funcs, type->element_type);
  }
}

static Str mangle_func_name(Func *func) {
  StringBuilder sb = {0};
  sb_push_str(&sb, func->expr->name);
  sb_push_type_hash(&sb, NULL, func->type);
  return sb_to_str(sb);
}

static void built_ins_to_gen_append(Encoder *encoder, Str name,
                                    u32 return_index, Indices arg_indices) {
  bool exists = false;

  for (u32 i = 0; i < encoder->built_ins_to_gen.len; ++i) {
    if (str_eq(encoder->built_ins_to_gen.items[i].name, name)) {
      exists = true;
      break;
    }
  }

  if (!exists) {
    Var *_return = NULL;
    if (return_index != (u32) -1)
      _return = encoder->vars->items + return_index;
    VarRefs args;
    args.len = arg_indices.len;
    args.cap = args.len;
    args.items = arena_alloc(encoder->arena, args.cap * sizeof(Var *));
    for (u32 i = 0; i < args.len; ++i)
      args.items[i] = encoder->vars->items + arg_indices.items[i];

    BuiltInToGen new_one = { name, _return, args, {} };
    DA_APPEND(encoder->built_ins_to_gen, new_one);
  }
}

static bool begins_with(Str str, Str prefix) {
  if (str.len < prefix.len)
    return false;

  str.len = prefix.len;
  return str_eq(str, prefix);
}

static bool type_is_free_generic(Type *type) {
  return type->kind == TypeKindFunc ||
         type->kind == TypeKindList;
}

static void built_ins_to_gen_append_rec(Encoder *encoder, Str name,
                                        u32 return_index, Indices arg_indices) {
  built_ins_to_gen_append(encoder, name, return_index, arg_indices);

  if (begins_with(name, STR_LIT("ether_get_value_len_as_str_5"))) {
    Type *element_type = encoder->vars->items[arg_indices.items[0]].type->element_type;
    if (!type_is_free_generic(element_type))
      return;

    Var var = {
      {},
      arena_alloc(encoder->arena, sizeof(Type)),
    };
    *var.type = *element_type;
    DA_APPEND(*encoder->vars, var);

    StringBuilder sb = {0};
    sb_push_str(&sb, STR_LIT("ether_get_value_len_as_str_"));
    sb_push_type_hash(&sb, NULL, var.type);

    Indices new_arg_indices;
    new_arg_indices.len = 1;
    new_arg_indices.cap = new_arg_indices.len;
    new_arg_indices.items = arena_alloc(encoder->arena, new_arg_indices.cap * sizeof(u32));
    new_arg_indices.items[0] = encoder->vars->len - 1;

    Str new_name;
    new_name.len = sb.len;
    new_name.ptr = arena_alloc(encoder->arena, new_name.len);
    memcpy(new_name.ptr, sb.buffer, new_name.len);
    free(sb.buffer);

    built_ins_to_gen_append_rec(encoder, new_name, return_index, new_arg_indices);
  } else if (begins_with(name, STR_LIT("ether_value_to_str_5"))) {
    Type *element_type = encoder->vars->items[arg_indices.items[2]].type->element_type;
    if (!type_is_free_generic(element_type))
      return;

    Var var = {
      {},
      arena_alloc(encoder->arena, sizeof(Type)),
    };
    *var.type = *element_type;
    DA_APPEND(*encoder->vars, var);

    StringBuilder sb = {0};
    sb_push_str(&sb, STR_LIT("ether_value_to_str_"));
    sb_push_type_hash(&sb, NULL, var.type);

    Indices new_arg_indices;
    new_arg_indices.len = 3;
    new_arg_indices.cap = new_arg_indices.len;
    new_arg_indices.items = arena_alloc(encoder->arena, new_arg_indices.cap * sizeof(u32));
    new_arg_indices.items[0] = arg_indices.items[0];
    new_arg_indices.items[1] = arg_indices.items[1];
    new_arg_indices.items[2] = encoder->vars->len - 1;

    Str new_name;
    new_name.len = sb.len;
    new_name.ptr = arena_alloc(encoder->arena, new_name.len);
    memcpy(new_name.ptr, sb.buffer, new_name.len);
    free(sb.buffer);

    built_ins_to_gen_append_rec(encoder, new_name, return_index, new_arg_indices);
  } else if (begins_with(name, STR_LIT("ether_rc_dec_1"))) {
    u32 func_index = encoder->vars->items[arg_indices.items[0]].type->func_index;
    Func *func = encoder->funcs->items + func_index;
    for (u32 i = 0; i < func->captured_vars.len; ++i) {
      Type *var_type = func->captured_vars.items[i].type;
      if (!type_is_free_generic(var_type))
        continue;

      Var var = { {}, var_type };
      DA_APPEND(*encoder->vars, var);

      StringBuilder sb = {0};
      sb_push_str(&sb, STR_LIT("ether_free_"));
      sb_push_type_hash(&sb, encoder->funcs, var_type);

      Indices new_arg_indices;
      new_arg_indices.len = 1;
      new_arg_indices.cap = new_arg_indices.len;
      new_arg_indices.items = arena_alloc(encoder->arena, new_arg_indices.cap * sizeof(u32));
      new_arg_indices.items[0] = encoder->vars->len - 1;

      Str new_name;
      new_name.len = sb.len;
      new_name.ptr = arena_alloc(encoder->arena, new_name.len);
      memcpy(new_name.ptr, sb.buffer, new_name.len);
      free(sb.buffer);

      built_ins_to_gen_append_rec(encoder, new_name, return_index, new_arg_indices);
    }
  } else if (begins_with(name, STR_LIT("ether_rc_dec_5"))) {
    StringBuilder sb = {0};
    sb_push_str(&sb, STR_LIT("ether_free_"));
    sb_push_type_hash(&sb, encoder->funcs, encoder->vars->items[arg_indices.items[0]].type);

    Indices new_arg_indices;
    new_arg_indices.len = 1;
    new_arg_indices.cap = new_arg_indices.len;
    new_arg_indices.items = arena_alloc(encoder->arena, new_arg_indices.cap * sizeof(u32));
    new_arg_indices.items[0] = arg_indices.items[0];

    Str new_name;
    new_name.len = sb.len;
    new_name.ptr = arena_alloc(encoder->arena, new_name.len);
    memcpy(new_name.ptr, sb.buffer, new_name.len);
    free(sb.buffer);

    built_ins_to_gen_append_rec(encoder, new_name, return_index, new_arg_indices);
  } else if (begins_with(name, STR_LIT("ether_free_5"))) {
    Type *element_type = encoder->vars->items[arg_indices.items[0]].type->element_type;
    if (!type_is_free_generic(element_type))
      return;

    Var var = {
      {},
      arena_alloc(encoder->arena, sizeof(Type)),
    };
    *var.type = *element_type;
    DA_APPEND(*encoder->vars, var);

    StringBuilder sb = {0};
    sb_push_str(&sb, STR_LIT("ether_free_"));
    sb_push_type_hash(&sb, encoder->funcs, var.type);

    Indices new_arg_indices;
    new_arg_indices.len = 1;
    new_arg_indices.cap = new_arg_indices.len;
    new_arg_indices.items = arena_alloc(encoder->arena, new_arg_indices.cap * sizeof(u32));
    new_arg_indices.items[0] = encoder->vars->len - 1;

    Str new_name;
    new_name.len = sb.len;
    new_name.ptr = arena_alloc(encoder->arena, new_name.len);
    memcpy(new_name.ptr, sb.buffer, new_name.len);
    free(sb.buffer);

    built_ins_to_gen_append_rec(encoder, new_name, return_index, new_arg_indices);
  }
}

static bool type_is_rc(Type *type) {
  return type->kind == TypeKindFunc ||
         type->kind == TypeKindStr ||
         type->kind == TypeKindList;
}

static void try_gen_rc_inc(Encoder *encoder, u32 index) {
  if (type_is_rc(encoder->vars->items[index].type)) {
    StringBuilder sb = {0};
    sb_push_str(&sb, STR_LIT("ether_rc_inc_"));
    sb_push_u32(&sb, encoder->vars->items[index].type->kind);

    Indices arg_indices;
    arg_indices.len = 1;
    arg_indices.cap = arg_indices.len;
    arg_indices.items = arena_alloc(encoder->arena, arg_indices.cap * sizeof(u32));
    arg_indices.items[0] = index;

    Str name;
    name.len = sb.len;
    name.ptr = arena_alloc(encoder->arena, name.len);
    memcpy(name.ptr, sb.buffer, name.len);
    free(sb.buffer);

    Instr instr = {
      InstrKindCall,
      {
        .call = {
          name,
          arg_indices,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);
  }
}

static void try_gen_rc_dec(Encoder *encoder, u32 index) {
  if (type_is_rc(encoder->vars->items[index].type)) {
    StringBuilder sb = {0};
    sb_push_str(&sb, STR_LIT("ether_rc_dec_"));
    sb_push_type_hash(&sb, encoder->funcs, encoder->vars->items[index].type);

    Str temp_name = { sb.buffer, STR_LIT("ether_rc_dec_").len + 1 };
    temp_name.ptr[STR_LIT("ether_rc_").len + 0] = 'i';
    temp_name.ptr[STR_LIT("ether_rc_").len + 1] = 'n';
    if (encoder->instrs.len > 0) {
      Instr *instr = encoder->instrs.items + encoder->instrs.len - 1;
      if (instr->kind == InstrKindCall && str_eq(instr->as.call.name, temp_name)) {
        free(sb.buffer);
        --encoder->instrs.len;
        return;
      }
    }
    temp_name.ptr[STR_LIT("ether_rc_").len + 0] = 'd';
    temp_name.ptr[STR_LIT("ether_rc_").len + 1] = 'e';

    Indices arg_indices;
    arg_indices.len = 1;
    arg_indices.cap = arg_indices.len;
    arg_indices.items = arena_alloc(encoder->arena, arg_indices.cap * sizeof(u32));
    arg_indices.items[0] = index;

    Str name;
    name.len = sb.len;
    name.ptr = arena_alloc(encoder->arena, name.len);
    memcpy(name.ptr, sb.buffer, name.len);
    free(sb.buffer);

    if (type_is_free_generic(encoder->vars->items[index].type))
      built_ins_to_gen_append_rec(encoder, name, (u32) -1, arg_indices);

    Instr instr = {
      InstrKindCall,
      {
        .call = {
          encoder->vars->items[index].type->kind == TypeKindFunc ?
            STR_LIT("ether_rc_dec_1") :
            name,
          arg_indices,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);
  }
}

static void get_captured_var(Encoder *encoder, u32 var_index, u32 dest_index) {
  Segments segments;
  segments.len = var_index + 2;
  segments.cap = segments.len;
  segments.items =
    arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));

  segments.items[0].offset = 0;
  segments.items[0].size = 8;

  i32 offset = 8;
  for (u32 i = 0; i <= var_index; ++i) {
    u32 size = get_type_size(encoder->vars->items[i].type);
    segments.items[i + 1].offset = offset;
    segments.items[i + 1].size = size;
    offset += size;
  }

  Var *var = encoder->vars->items + var_index;
  ValueKind kind = get_type_value_kind(var->type);
  u32 size = get_type_size(var->type);

  Instr instr = {
    InstrKindCopyFromRefFixed,
    {
      .copy_from_ref_fixed = {
        dest_index,
        0,
        segments,
        kind,
        size,
        true,
        false,
      },
    },
  };
  DA_APPEND(encoder->instrs, instr);
}

static void encode_block(Encoder *encoder, Exprs *block,
                         u32 dest_index, bool last_is_return);

static void encode_expr(Encoder *encoder, Expr *expr, u32 dest_index) {
  switch (expr->kind) {
  case ExprKindStr: {
    if (dest_index == (u32) -1)
      break;

    Instr instr = {
      InstrKindStoreData,
      {
        .store_data = {
          dest_index,
          encoder->data.len,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    DataEntry entry = {
      arena_alloc(encoder->arena, expr->as.str.str.len),
      expr->as.str.str.len,
    };
    memcpy(entry.data, expr->as.str.str.ptr, entry.len);
    DA_APPEND(encoder->data, entry);
  } break;

  case ExprKindInt: {
    if (dest_index == (u32) -1)
      break;

    Instr instr = {
      InstrKindStore,
      {
        .store = {
          dest_index,
          {
            ValueKindSigned,
            {
              ._signed = expr->as._int._int,
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);
  } break;

  case ExprKindBool: {
    Instr instr = {
      InstrKindStore,
      {
        .store = {
          dest_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = expr->as._bool._bool,
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);
  } break;

  case ExprKindBlock: {
    encode_block(encoder, &expr->as.block, dest_index, false);
  } break;

  case ExprKindIdent: {
    if (dest_index == (u32) -1)
      break;

    u32 index = get_var_index(encoder->vars, encoder->vars_defined, expr->as.ident.name);
    if (index == (u32) -1) {
      index = get_func_index(encoder->funcs, expr->as.ident.name);
      Func *func = encoder->funcs->items + index;
      Str name = func->expr->name;
      if (!str_eq(name, STR_LIT("main")) && !func->is_lambda) {
        Str temp_name = mangle_func_name(encoder->funcs->items + index);
        name.len = temp_name.len;
        name.ptr = arena_alloc(encoder->arena, name.len);
        memcpy(name.ptr, temp_name.ptr, name.len);
        free(temp_name.ptr);
      }

      Instr instr = {
        InstrKindRefProc,
        {
          .ref_proc = {
            dest_index,
            name,
          },
        },
      };
      DA_APPEND(encoder->instrs, instr);
    } else {
      Instr instr = {
        InstrKindCopy,
        {
          .copy = {
            dest_index,
            index,
          },
        },
      };
      DA_APPEND(encoder->instrs, instr);
    }
  } break;

  case ExprKindFunc: {
    if (dest_index == (u32) -1)
      break;

    Instr instr = {
      InstrKindRefProc,
      {
        .ref_proc = {
          dest_index,
          expr->as.func.name,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    u32 func_index = get_func_index(encoder->funcs, expr->as.func.name);
    Func *func = encoder->funcs->items + func_index;

    if (func->captured_vars.len == 0)
      break;

    u32 ctx_size = 0;
    for (u32 i = 0; i < func->captured_vars.len; ++i)
      ctx_size += get_type_size(func->captured_vars.items[i].type);

    u32 size_index = define_var(encoder);
    u32 ctx_index = define_var(encoder);
    u32 free_func_index = define_var(encoder);

    instr = (Instr) {
      InstrKindStore,
      {
        .store = {
          size_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = ctx_size,
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    Var *ctx_var = encoder->vars->items + ctx_index;
    ValueKind kind = get_type_value_kind(ctx_var->type);
    u32 size = get_type_size(ctx_var->type);

    Indices arg_indices;
    arg_indices.len = 1;
    arg_indices.cap = arg_indices.len;
    arg_indices.items = arena_alloc(encoder->arena, arg_indices.cap * sizeof(u32));
    arg_indices.items[0] = size_index;

    instr = (Instr) {
      InstrKindCallAssign,
      {
        .call_assign = {
          ctx_index,
          size,
          kind,
          STR_LIT("ether_alloc"),
          arg_indices,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    StringBuilder sb = {0};
    sb_push_str(&sb, STR_LIT("ether_rc_dec_"));
    sb_push_type_hash(&sb, encoder->funcs, func->type);

    Str name;
    name.len = sb.len;
    name.ptr = arena_alloc(encoder->arena, name.len);
    memcpy(name.ptr, sb.buffer, name.len);
    free(sb.buffer);

    instr = (Instr) {
      InstrKindRefProc,
      {
        .ref_proc = {
          free_func_index,
          name,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    arg_indices.len = 1;
    arg_indices.cap = arg_indices.len;
    arg_indices.items = arena_alloc(encoder->arena, arg_indices.cap * sizeof(u32));
    arg_indices.items[0] = dest_index;

    Segments segments;
    segments.len = 1;
    segments.cap = segments.len;
    segments.items =
      arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));
    segments.items[0].offset = 0;
    segments.items[0].size = 8;

    instr = (Instr) {
      InstrKindCopyToRefFixed,
      {
        .copy_to_ref_fixed = {
          ctx_index,
          segments,
          free_func_index,
          true,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    for (u32 i = 0; i < func->captured_vars.len; ++i) {
      Segments segments;
      segments.len = i + 2;
      segments.cap = segments.len;
      segments.items =
        arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));

      segments.items[0].offset = 0;
      segments.items[0].size = 8;

      i32 offset = 8;
      for (u32 j = 0; j <= i; ++j) {
        u32 size = get_type_size(func->captured_vars.items[j].type);
        segments.items[j + 1].offset = offset;
        segments.items[j + 1].size = size;
        offset += size;
      }

      u32 index = get_var_index(encoder->vars, encoder->vars_defined,
                                func->captured_vars.items[i].name);

      try_gen_rc_inc(encoder, index);

      instr = (Instr) {
        InstrKindCopyToRefFixed,
        {
          .copy_to_ref_fixed = {
            ctx_index,
            segments,
            index,
            true,
          },
        },
      };
      DA_APPEND(encoder->instrs, instr);
    }

    segments.len = 2;
    segments.cap = segments.len;
    segments.items =
      arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));
    segments.items[0].offset = 0;
    segments.items[0].size = 8;
    segments.items[1].offset = 8;
    segments.items[1].size = 8;

    instr = (Instr) {
      InstrKindCopyToRefFixed,
      {
        .copy_to_ref_fixed = {
          dest_index,
          segments,
          ctx_index,
          false,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);
  } break;

  case ExprKindFuncCall: {
    bool opt = expr->as.func_call.func->kind == ExprKindIdent;
    if (opt)
      opt = get_var_index(encoder->vars, encoder->vars_defined,
                          expr->as.func_call.func->as.ident.name) == (u32) -1;

    u32 index;
    if (!opt) {
      index = define_var(encoder);
      encode_expr(encoder, expr->as.func_call.func, index);
    }

    Str name;
    bool is_built_in_to_gen = false;
    if (opt) {
      if (expr->as.func_call.built_in) {
        name = expr->as.func_call.built_in->name;
      } else {
        u32 func_index = get_func_index(encoder->funcs, expr->as.func_call.func->as.ident.name);
        Func *func = encoder->funcs->items + func_index;
        if (!func->is_lambda) {
          Str temp_name = mangle_func_name(func);
          name.len = temp_name.len;
          name.ptr = arena_alloc(encoder->arena, name.len);
          memcpy(name.ptr, temp_name.ptr, name.len);
          free(temp_name.ptr);
        }
      }

      is_built_in_to_gen = str_eq(name, STR_LIT("prep")) ||
                           str_eq(name, STR_LIT("app"));
    }

    bool is_closure = !expr->as.func_call.built_in;

    Indices arg_indices;
    arg_indices.len = expr->as.func_call.args.len + is_closure;
    arg_indices.cap = arg_indices.len;
    arg_indices.items = arena_alloc(encoder->arena, arg_indices.cap * sizeof(u32));

    if (is_closure) {
      u32 ctx_index = define_var(encoder);
      Var *ctx_var = encoder->vars->items + ctx_index;

      ValueKind kind = get_type_value_kind(ctx_var->type);
      u32 size = get_type_size(ctx_var->type);

      if (!opt) {
        Segments segments;
        segments.len = 2;
        segments.cap = segments.len;
        segments.items =
          arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));
        segments.items[0].offset = 0;
        segments.items[0].size = 8;
        segments.items[1].offset = 8;
        segments.items[1].size = 8;

        Instr instr = {
          InstrKindCopyFromRefFixed,
          {
            .copy_from_ref_fixed = {
              ctx_index,
              index,
              segments,
              kind,
              size,
              false,
              false,
            },
          },
        };
        DA_APPEND(encoder->instrs, instr);
      }

      arg_indices.items[0] = ctx_index;
    }

    for (u32 i = 0; i < expr->as.func_call.args.len; ++i) {
      u32 arg_index = define_var(encoder);
      encode_expr(encoder, expr->as.func_call.args.items[i], arg_index);
      if (!is_built_in_to_gen)
        try_gen_rc_inc(encoder, arg_index);
      arg_indices.items[i + is_closure] = arg_index;
    }

    if (is_built_in_to_gen) {
      StringBuilder sb = {0};
      sb_push_str(&sb, name);
      sb_push_char(&sb, '_');
      if (str_eq(name, STR_LIT("prep")))
        sb_push_type_hash(&sb, NULL, encoder->vars->items[arg_indices.items[1]].type);
      else
        sb_push_type_hash(&sb, NULL, encoder->vars->items[arg_indices.items[0]].type);

      name.len = sb.len;
      name.ptr = arena_alloc(encoder->arena, name.len);
      memcpy(name.ptr, sb.buffer, name.len);
      free(sb.buffer);

      built_ins_to_gen_append(encoder, name, dest_index, arg_indices);
    }

    Instr instr;
    if (opt) {
      if (dest_index == (u32) -1) {
        instr = (Instr) {
          InstrKindCall,
          {
            .call = {
              name,
              arg_indices,
            },
          },
        };
      } else {
        Var *var = encoder->vars->items + dest_index;
        u32 size = get_type_size(var->type);
        ValueKind kind = get_type_value_kind(var->type);

        instr = (Instr) {
          InstrKindCallAssign,
          {
            .call_assign = {
              dest_index,
              size,
              kind,
              name,
              arg_indices,
            },
          },
        };
      }
    } else {
      if (dest_index == (u32) -1) {
        instr = (Instr) {
          InstrKindCallRef,
          {
            .call_ref = {
              index,
              arg_indices,
            },
          },
        };
      } else {
        Var *var = encoder->vars->items + dest_index;
        u32 size = get_type_size(var->type);
        ValueKind kind = get_type_value_kind(var->type);

        instr = (Instr) {
          InstrKindCallRefAssign,
          {
            .call_ref_assign = {
              dest_index,
              size,
              kind,
              index,
              arg_indices,
            },
          },
        };
      }
    }
    DA_APPEND(encoder->instrs, instr);

    if (!is_built_in_to_gen)
      for (u32 i = expr->as.func_call.args.len; i > 0; --i)
        try_gen_rc_dec(encoder, arg_indices.items[i - 1]);
  } break;

  case ExprKindLet: {
    u32 index = define_var(encoder);
    encode_expr(encoder, expr->as.let.value, index);

    try_gen_rc_inc(encoder, index);

    if (dest_index != (u32) -1) {
      Instr instr = {
        InstrKindCopy,
        {
          .copy = {
            dest_index,
            index,
          },
        },
      };
      DA_APPEND(encoder->instrs, instr);
    }
  } break;

  case ExprKindSet: {
    u32 index = get_var_index(encoder->vars, encoder->vars_defined, expr->as.set.name);

    try_gen_rc_dec(encoder, index);
    encode_expr(encoder, expr->as.set.value, index);
    try_gen_rc_inc(encoder, index);

    if (dest_index != (u32) -1) {
      Instr instr = {
        InstrKindCopy,
        {
          .copy = {
            dest_index,
            index,
          },
        },
      };
      DA_APPEND(encoder->instrs, instr);
    }
  } break;

  case ExprKindRet: {
    if (expr->as.ret.value) {
      u32 index = define_var(encoder);
      encode_expr(encoder, expr->as.ret.value, index);

      Instr instr = {
        InstrKindRetVal,
        {
          .ret_val = { index },
        },
      };
      DA_APPEND(encoder->instrs, instr);
    } else {
      Instr instr = {
        InstrKindRet,
        {},
      };
      DA_APPEND(encoder->instrs, instr);
    }
  } break;

  case ExprKindIf: {
    u32 cond_index = define_var(encoder);
    encode_expr(encoder, expr->as._if.cond, cond_index);

    u32 cond_jump_instr_index = encoder->instrs.len;
    Instr instr = {
      InstrKindJumpIfNot,
      {
        .jump_if_not = { cond_index, 0 },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    encode_block(encoder, &expr->as._if.if_body, dest_index, false);

    u32 jump_instr_index = encoder->instrs.len;
    if (expr->as._if.else_body.len > 0) {
      Instr instr = {
        InstrKindJump,
        {
          .jump = { 0 },
        },
      };
      DA_APPEND(encoder->instrs, instr);
    }

    encoder->instrs.items[cond_jump_instr_index].as.jump_if_not.target = encoder->instrs.len;

    encode_block(encoder, &expr->as._if.else_body, dest_index, false);

    if (expr->as._if.else_body.len > 0)
      encoder->instrs.items[jump_instr_index].as.jump.target = encoder->instrs.len;
  } break;

  case ExprKindBinOp: {
    if (dest_index == (u32) -1)
      break;

    u32 temp_dest_index = dest_index;
    if (expr->as.bin_op.args.len > 2)
      temp_dest_index = define_var(encoder);

    u32 index0 = define_var(encoder);
    encode_expr(encoder, expr->as.bin_op.args.items[0], index0);
    u32 index1 = define_var(encoder);
    encode_expr(encoder, expr->as.bin_op.args.items[1], index1);

    BinOpKind kind = bin_op_kind_to_evm(expr->as.bin_op.kind);

    Instr instr = {
      InstrKindBinOp,
      {
        .bin_op = {
          temp_dest_index,
          index0,
          index1,
          kind,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    for (u32 i = 2; i < expr->as.bin_op.args.len; ++i) {
      u32 index = define_var(encoder);
      encode_expr(encoder, expr->as.bin_op.args.items[i], index);

      Instr instr = {
        InstrKindBinOp,
        {
          .bin_op = {
            temp_dest_index,
            temp_dest_index,
            index,
            kind,
          },
        },
      };
      DA_APPEND(encoder->instrs, instr);
    }

    if (expr->as.bin_op.args.len > 2) {
      Instr instr = {
        InstrKindCopy,
        {
          .copy = {
            dest_index,
            temp_dest_index,
          },
        },
      };
      DA_APPEND(encoder->instrs, instr);
    }
  } break;

  case ExprKindFStr: {
    if (dest_index == (u32) -1)
      break;

    // Size (because strings are pascal-like), null-terminator and non-static marker
    u32 start_size = 6;
    Indices part_var_indices = {0};

    for (u32 i = 0; i < expr->as.fstr.parts.len; ++i) {
      FStrPart *part = expr->as.fstr.parts.items + i;
      if (part->expr) {
        u32 part_var_index = define_var(encoder);
        encode_expr(encoder, part->expr, part_var_index);
        DA_APPEND(part_var_indices, part_var_index);
      } else {
        start_size += part->str.len;
        DA_APPEND(part_var_indices, (u32) -1);
      }
    }

    u32 size_index = define_var(encoder);
    Instr instr = {
      InstrKindStore,
      {
        .store = {
          size_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = start_size,
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    u32 temp_size_index = define_var(encoder);

    for (u32 i = 0; i < expr->as.fstr.parts.len; ++i) {
      FStrPart *part = expr->as.fstr.parts.items + i;
      if (!part->expr)
        continue;

      u32 part_var_index = part_var_indices.items[i];

      Indices arg_indices;
      arg_indices.len = 1;
      arg_indices.cap = arg_indices.len;
      arg_indices.items = arena_alloc(encoder->arena, arg_indices.cap * sizeof(u32));
      arg_indices.items[0] = part_var_index;

      StringBuilder sb = {0};
      sb_push_str(&sb, STR_LIT("ether_get_value_len_as_str_"));
      sb_push_type_hash(&sb, NULL, encoder->vars->items[part_var_index].type);

      Str name;
      name.len = sb.len;
      name.ptr = arena_alloc(encoder->arena, name.len);
      memcpy(name.ptr, sb.buffer, name.len);
      free(sb.buffer);

      if (encoder->vars->items[part_var_index].type->kind == TypeKindList)
        built_ins_to_gen_append_rec(encoder, name, temp_size_index, arg_indices);

      instr = (Instr) {
        InstrKindCallAssign,
        {
          .call_assign = {
            temp_size_index,
            8,
            ValueKindUnsigned,
            name,
            arg_indices,
          },
        },
      };
      DA_APPEND(encoder->instrs, instr);

      instr = (Instr) {
        InstrKindBinOp,
        {
          .bin_op = {
            size_index,
            size_index,
            temp_size_index,
            BinOpKindAddInt,
          },
        },
      };
      DA_APPEND(encoder->instrs, instr);
    }

    Var *var = encoder->vars->items + dest_index;
    u32 size = get_type_size(var->type);
    ValueKind kind = get_type_value_kind(var->type);

    Indices arg_indices;
    arg_indices.len = 1;
    arg_indices.cap = arg_indices.len;
    arg_indices.items = arena_alloc(encoder->arena, arg_indices.cap * sizeof(u32));
    arg_indices.items[0] = size_index;

    instr = (Instr) {
      InstrKindCallAssign,
      {
        .call_assign = {
          dest_index,
          size,
          kind,
          STR_LIT("ether_alloc"),
          arg_indices,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindStore,
      {
        .store = {
          temp_size_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = 6,
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindBinOp,
      {
        .bin_op = {
          size_index,
          size_index,
          temp_size_index,
          BinOpKindSubInt,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    Segments segments;
    segments.len = 1;
    segments.cap = segments.len;
    segments.items = arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));
    segments.items[0].offset = 0;
    segments.items[0].size = 8;

    instr = (Instr) {
      InstrKindCopyToRefFixed,
      {
        .copy_to_ref_fixed = {
          dest_index,
          segments,
          size_index,
          true,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindStore,
      {
        .store = {
          size_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = 4,
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    for (u32 i = 0; i < expr->as.fstr.parts.len; ++i) {
      FStrPart *part = expr->as.fstr.parts.items + i;

      Indices arg_indices;
      arg_indices.len = 3;
      arg_indices.cap = arg_indices.len;
      arg_indices.items = arena_alloc(encoder->arena, arg_indices.cap * sizeof(u32));
      arg_indices.items[0] = dest_index;
      arg_indices.items[1] = size_index;

      StringBuilder sb = {0};
      if (part->expr) {
        u32 part_var_index = part_var_indices.items[i];

        arg_indices.items[2] = part_var_index;

        sb_push_str(&sb, STR_LIT("ether_value_to_str_"));
        sb_push_type_hash(&sb, NULL, encoder->vars->items[part_var_index].type);
      } else {
        instr = (Instr) {
          InstrKindStoreData,
          {
            .store_data = {
              temp_size_index,
              encoder->data.len,
            },
          },
        };
        DA_APPEND(encoder->instrs, instr);

        DataEntry entry = {
          arena_alloc(encoder->arena, part->str.len),
          part->str.len,
        };
        memcpy(entry.data, part->str.ptr, entry.len);
        DA_APPEND(encoder->data, entry);

        arg_indices.items[2] = temp_size_index;

        Type str_type = { TypeKindStr, {} };

        sb_push_str(&sb, STR_LIT("ether_value_to_str_"));
        sb_push_type_hash(&sb, NULL, &str_type);
      }

      Str name;
      name.len = sb.len;
      name.ptr = arena_alloc(encoder->arena, name.len);
      memcpy(name.ptr, sb.buffer, name.len);
      free(sb.buffer);

      if (part->expr &&
          encoder->vars->items[arg_indices.items[2]].type->kind == TypeKindList)
        built_ins_to_gen_append_rec(encoder, name, temp_size_index, arg_indices);

      instr = (Instr) {
        InstrKindCallAssign,
        {
          .call_assign = {
            temp_size_index,
            8,
            ValueKindUnsigned,
            name,
            arg_indices,
          },
        },
      };
      DA_APPEND(encoder->instrs, instr);

      if (i + 1 < expr->as.fstr.parts.len) {
        instr = (Instr) {
          InstrKindBinOp,
          {
            .bin_op = {
              size_index,
              size_index,
              temp_size_index,
              BinOpKindAddInt,
            },
          },
        };
        DA_APPEND(encoder->instrs, instr);
      }
    }

    if (part_var_indices.items)
      free(part_var_indices.items);
  } break;

  case ExprKindList: {
    if (dest_index == (u32) -1)
      return;

    Exprs *elements = &expr->as.list.elements;

    if (elements->len == 0) {
      Instr instr = {
        InstrKindStore,
        {
          .store = {
            dest_index,
            {
              ValueKindUnsigned,
              {
                ._unsigned = 0,
              },
            },
          },
        },
      };
      DA_APPEND(encoder->instrs, instr);

      break;
    }

    Var *var = encoder->vars->items + dest_index;
    u32 size = get_type_size(var->type);
    ValueKind kind = get_type_value_kind(var->type);

    u32 element_index = define_var(encoder);
    u32 size_index = define_var(encoder);

    u32 element_size = get_type_size(encoder->vars->items[element_index].type);
    Instr instr = {
      InstrKindStore,
      {
        .store = {
          size_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = 8 + element_size,
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    Indices arg_indices;
    arg_indices.len = 2;
    arg_indices.cap = arg_indices.len;
    arg_indices.items = arena_alloc(encoder->arena, arg_indices.cap * sizeof(u32));
    arg_indices.items[0] = element_index;
    arg_indices.items[1] = dest_index;

    StringBuilder sb = {0};
    sb_push_str(&sb, STR_LIT("prep_"));
    sb_push_type_hash(&sb, NULL, encoder->vars->items[element_index].type);

    Str name;
    name.len = sb.len;
    name.ptr = arena_alloc(encoder->arena, name.len);
    memcpy(name.ptr, sb.buffer, name.len);
    free(sb.buffer);

    built_ins_to_gen_append(encoder, name, dest_index, arg_indices);

    instr = (Instr) {
      InstrKindStore,
      {
        .store = {
          dest_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = 0,
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    for (u32 i = elements->len; i > 0; --i) {
      encode_expr(encoder, elements->items[i - 1], element_index);

      instr = (Instr) {
        InstrKindCallAssign,
        {
          .call_assign = {
            dest_index,
            size,
            kind,
            name,
            arg_indices,
          },
        },
      };
      DA_APPEND(encoder->instrs, instr);
    }
  } break;
  }
}

static void encode_block(Encoder *encoder, Exprs *block,
                         u32 dest_index, bool skip_last) {
  for (u32 i = 0; i + 1 < block->len; ++i)
    encode_expr(encoder, block->items[i], (u32) -1);

  if (block->len > 0)
    if (!skip_last || block->items[block->len - 1]->kind == ExprKindRet)
      encode_expr(encoder, block->items[block->len - 1], dest_index);
}

static void encode_instr(FILE *stream, Instr *instr) {
  u8 kind = instr->kind;
  fwrite(&kind, 1, 1, stream);

  switch (instr->kind) {
  case InstrKindAlloc: {
    fwrite(&instr->as.alloc.index, sizeof(instr->as.alloc.index), 1, stream);
    fwrite(&instr->as.alloc.segments.len, sizeof(instr->as.alloc.segments.len), 1, stream);
    for (u32 k = 0; k < instr->as.alloc.segments.len; ++k) {
      AlignedSegment *segment = instr->as.alloc.segments.items + k;
      fwrite(&segment->offset, sizeof(segment->offset), 1, stream);
      fwrite(&segment->size, sizeof(segment->size), 1, stream);
    }
  } break;

  case InstrKindStore: {
    fwrite(&instr->as.store.index, sizeof(instr->as.store.index), 1, stream);
    u8 value_kind = instr->as.store.value.kind;
    fwrite(&value_kind, 1, 1, stream);
    switch (instr->as.store.value.kind) {
    case ValueKindSigned: {
      i64 _signed = instr->as.store.value.as._signed;
      fwrite(&_signed, sizeof(_signed), 1, stream);
    } break;

    case ValueKindUnsigned: {
      u64 _unsigned = instr->as.store.value.as._unsigned;
      fwrite(&_unsigned, sizeof(_unsigned), 1, stream);
    } break;
    }
  } break;

  case InstrKindCopy: {
    fwrite(&instr->as.copy.dest_index, sizeof(instr->as.copy.dest_index), 1, stream);
    fwrite(&instr->as.copy.src_index, sizeof(instr->as.copy.src_index), 1, stream);
  } break;

  case InstrKindBinOp: {
    fwrite(&instr->as.bin_op.dest_index, sizeof(instr->as.bin_op.dest_index), 1, stream);
    fwrite(&instr->as.bin_op.src0_index, sizeof(instr->as.bin_op.src0_index), 1, stream);
    fwrite(&instr->as.bin_op.src1_index, sizeof(instr->as.bin_op.src1_index), 1, stream);
    u8 bin_op_kind = instr->as.bin_op.kind;
    fwrite(&bin_op_kind, 1, 1, stream);
  } break;

  case InstrKindCall: {
    encode_str(stream, instr->as.call.name);
    fwrite(&instr->as.call.arg_indices.len, sizeof(instr->as.call.arg_indices.len), 1, stream);
    for (u32 k = 0; k < instr->as.call.arg_indices.len; ++k) {
      u32 arg_index = instr->as.call.arg_indices.items[k];
      fwrite(&arg_index, sizeof(arg_index), 1, stream);
    }
  } break;

  case InstrKindCallAssign: {
    fwrite(&instr->as.call_assign.dest_index, sizeof(instr->as.call_assign.dest_index), 1, stream);
    fwrite(&instr->as.call_assign.return_size, sizeof(instr->as.call_assign.return_size), 1, stream);
    fwrite(&instr->as.call_assign.return_kind, 1, 1, stream);

    encode_str(stream, instr->as.call_assign.name);
    fwrite(&instr->as.call_assign.arg_indices.len, sizeof(instr->as.call_assign.arg_indices.len), 1, stream);
    for (u32 i = 0; i < instr->as.call_assign.arg_indices.len; ++i) {
      u32 arg_index = instr->as.call_assign.arg_indices.items[i];
      fwrite(&arg_index, sizeof(arg_index), 1, stream);
    }
  } break;

  case InstrKindRet: break;

  case InstrKindRetVal: {
    fwrite(&instr->as.ret_val.index, sizeof(instr->as.ret_val.index), 1, stream);
  } break;

  case InstrKindJump: {
    fwrite(&instr->as.jump.target, sizeof(instr->as.jump.target), 1, stream);
  } break;

  case InstrKindJumpIfNot: {
    fwrite(&instr->as.jump_if_not.cond_index, sizeof(instr->as.jump_if_not.cond_index), 1, stream);
    fwrite(&instr->as.jump_if_not.target, sizeof(instr->as.jump_if_not.target), 1, stream);
  } break;

  case InstrKindRef: {
    fprintf(stderr, "Instr not yet implemented: %u\n", instr->kind);
    exit(1);
  }

  case InstrKindCopyToRef: {
    fwrite(&instr->as.copy_to_ref.dest_index, sizeof(instr->as.copy_to_ref.dest_index), 1, stream);
    u8 one = 1;
    fwrite(&one, 1, 1, stream);
    fwrite(&instr->as.copy_to_ref.dest_offset_index, sizeof(instr->as.copy_to_ref.dest_offset_index), 1, stream);
    fwrite(&instr->as.copy_to_ref.src_index, sizeof(instr->as.copy_to_ref.src_index), 1, stream);
  } break;

  case InstrKindCopyFromRef:
  case InstrKindInlineAsm: {
    fprintf(stderr, "Instr not yet implemented: %u\n", instr->kind);
    exit(1);
  }

  case InstrKindStoreData: {
    fwrite(&instr->as.store_data.index, sizeof(instr->as.store_data.index), 1, stream);
    fwrite(&instr->as.store_data.data_index, sizeof(instr->as.store_data.data_index), 1, stream);
  } break;

  case InstrKindConvert: {
    fwrite(&instr->as.convert.dest_index, sizeof(instr->as.convert.dest_index), 1, stream);
    fwrite(&instr->as.convert.dest_kind, 1, 1, stream);
    fwrite(&instr->as.convert.dest_size, sizeof(instr->as.convert.dest_size), 1, stream);
    fwrite(&instr->as.convert.src_index, sizeof(instr->as.convert.src_index), 1, stream);
  } break;

  case InstrKindCopyToRefFixed: {
    fwrite(&instr->as.copy_to_ref_fixed.dest_index, sizeof(instr->as.copy_to_ref_fixed.dest_index), 1, stream);

    Segments *segments = &instr->as.copy_to_ref_fixed.dest_segments;
    fwrite(&segments->len, sizeof(segments->len), 1, stream);
    for (u32 i = 0; i < segments->len; ++i) {
      fwrite(&segments->items[i].offset, sizeof(segments->items[i].offset), 1, stream);
      fwrite(&segments->items[i].size, sizeof(segments->items[i].size), 1, stream);
    }

    fwrite(&instr->as.copy_to_ref_fixed.src_index, sizeof(instr->as.copy_to_ref_fixed.src_index), 1, stream);

    fwrite(&instr->as.copy_to_ref_fixed.deref, 1, 1, stream);
  } break;

  case InstrKindCopyFromRefFixed: {
    fwrite(&instr->as.copy_from_ref_fixed.dest_index, sizeof(instr->as.copy_from_ref_fixed.dest_index), 1, stream);

    fwrite(&instr->as.copy_from_ref_fixed.src_index, sizeof(instr->as.copy_from_ref_fixed.src_index), 1, stream);

    Segments *segments = &instr->as.copy_from_ref_fixed.src_segments;
    fwrite(&segments->len, sizeof(segments->len), 1, stream);
    for (u32 i = 0; i < segments->len; ++i) {
      fwrite(&segments->items[i].offset, sizeof(segments->items[i].offset), 1, stream);
      fwrite(&segments->items[i].size, sizeof(segments->items[i].size), 1, stream);
    }

    fwrite(&instr->as.copy_from_ref_fixed.src_target_kind, 1, 1, stream);
    fwrite(&instr->as.copy_from_ref_fixed.src_target_size, sizeof(instr->as.copy_from_ref_fixed.src_target_size), 1, stream);

    fwrite(&instr->as.copy_from_ref_fixed.deref, 1, 1, stream);
    fwrite(&instr->as.copy_from_ref_fixed.take_ref, 1, 1, stream);
  } break;

  case InstrKindRefProc: {
    fwrite(&instr->as.ref_proc.dest_index, sizeof(instr->as.ref_proc.dest_index), 1, stream);
    encode_str(stream, instr->as.ref_proc.proc_name);
  } break;

  case InstrKindCallRef: {
    fwrite(&instr->as.call_ref.index, sizeof(instr->as.call_ref.index), 1, stream);
    fwrite(&instr->as.call_ref.arg_indices.len, sizeof(instr->as.call_ref.arg_indices.len), 1, stream);
    for (u32 i = 0; i < instr->as.call_ref.arg_indices.len; ++i) {
      u32 arg_index = instr->as.call_ref.arg_indices.items[i];
      fwrite(&arg_index, sizeof(arg_index), 1, stream);
    }
  } break;

  case InstrKindCallRefAssign: {
    fwrite(&instr->as.call_ref_assign.dest_index, sizeof(instr->as.call_ref_assign.dest_index), 1, stream);
    fwrite(&instr->as.call_ref_assign.return_size, sizeof(instr->as.call_ref_assign.return_size), 1, stream);
    u8 return_kind = instr->as.call_ref_assign.return_kind;
    fwrite(&return_kind, 1, 1, stream);

    fwrite(&instr->as.call_ref_assign.index, sizeof(instr->as.call_ref_assign.index), 1, stream);
    fwrite(&instr->as.call_ref_assign.arg_indices.len, sizeof(instr->as.call_ref_assign.arg_indices.len), 1, stream);
    for (u32 i = 0; i < instr->as.call_ref_assign.arg_indices.len; ++i) {
      u32 arg_index = instr->as.call_ref_assign.arg_indices.items[i];
      fwrite(&arg_index, sizeof(arg_index), 1, stream);
    }
  } break;
  }
}

static void encode_built_in_to_gen(Encoder *encoder, BuiltInToGen *built_in) {
  if (begins_with(built_in->name, STR_LIT("prep"))) {
    Var cond_var = {
      {},
      arena_alloc(encoder->arena, sizeof(Type)),
    };
    cond_var.type->kind = TypeKindInt;
    DA_APPEND(*encoder->vars, cond_var);
    u32 cond_index = define_var(encoder);

    Var zero_var = {
      {},
      arena_alloc(encoder->arena, sizeof(Type)),
    };
    zero_var.type->kind = TypeKindInt;
    DA_APPEND(*encoder->vars, zero_var);
    u32 zero_index = define_var(encoder);

    Instr instr = {
      InstrKindStore,
      {
        .store = {
          zero_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = 0,
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindBinOp,
      {
        .bin_op = {
          cond_index,
          1,
          zero_index,
          BinOpKindNeInt,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    u32 jump_instr_index = encoder->instrs.len;
    instr = (Instr) {
      InstrKindJumpIfNot,
      {
        .jump_if_not = {
          cond_index,
          0,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    try_gen_rc_inc(encoder, 1);

    encoder->instrs.items[jump_instr_index].as.jump_if_not.target = encoder->instrs.len;

    Var dest_var = {
      {},
      built_in->_return->type,
    };
    DA_APPEND(*encoder->vars, dest_var);
    u32 dest_index = define_var(encoder);

    Var size_var = {
      {},
      arena_alloc(encoder->arena, sizeof(Type)),
    };
    size_var.type->kind = TypeKindInt;
    DA_APPEND(*encoder->vars, size_var);
    u32 size_index = define_var(encoder);

    u32 element_size = get_type_size(encoder->vars->items[0].type);
    instr = (Instr) {
      InstrKindStore,
      {
        .store = {
          size_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = 8 + element_size,
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    u32 size = get_type_size(built_in->_return->type);
    ValueKind kind = get_type_value_kind(built_in->_return->type);

    Indices arg_indices;
    arg_indices.len = 1;
    arg_indices.cap = arg_indices.len;
    arg_indices.items = arena_alloc(encoder->arena, arg_indices.cap * sizeof(u32));
    arg_indices.items[0] = size_index;

    instr = (Instr) {
      InstrKindCallAssign,
      {
        .call_assign = {
          dest_index,
          size,
          kind,
          STR_LIT("ether_alloc"),
          arg_indices,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    Segments segments;
    segments.len = 2;
    segments.cap = segments.len;
    segments.items = arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));
    segments.items[0].offset = 0;
    segments.items[0].size = 8;
    segments.items[1].offset = 8;
    segments.items[1].size = element_size;

    instr = (Instr) {
      InstrKindCopyToRefFixed,
      {
        .copy_to_ref_fixed = {
          dest_index,
          segments,
          0,
          true,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    segments.len = 1;
    segments.cap = segments.len;
    segments.items = arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));
    segments.items[0].offset = 0;
    segments.items[0].size = 8;

    instr = (Instr) {
      InstrKindCopyToRefFixed,
      {
        .copy_to_ref_fixed = {
          dest_index,
          segments,
          1,
          true,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindRetVal,
      {
        .ret_val = {
          dest_index,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);
  } else if (begins_with(built_in->name, STR_LIT("app"))) {
    Var new_var = {
      {},
      built_in->_return->type,
    };
    DA_APPEND(*encoder->vars, new_var);
    u32 new_index = define_var(encoder);

    Var temp_var = {
      {},
      built_in->_return->type,
    };
    DA_APPEND(*encoder->vars, temp_var);
    u32 temp_index = define_var(encoder);

    Instr instr = {
      InstrKindCopy,
      {
        .copy = {
          temp_index,
          0,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    Var cond_var = {
      {},
      arena_alloc(encoder->arena, sizeof(Type)),
    };
    cond_var.type->kind = TypeKindInt;
    DA_APPEND(*encoder->vars, cond_var);
    u32 cond_index = define_var(encoder);

    Var zero_var = {
      {},
      arena_alloc(encoder->arena, sizeof(Type)),
    };
    zero_var.type->kind = TypeKindInt;
    DA_APPEND(*encoder->vars, zero_var);
    u32 zero_index = define_var(encoder);

    instr = (Instr) {
      InstrKindStore,
      {
        .store = {
          zero_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = 0,
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    u32 cond_instr_index = encoder->instrs.len;

    instr = (Instr) {
      InstrKindBinOp,
      {
        .bin_op = {
          cond_index,
          temp_index,
          zero_index,
          BinOpKindNeInt,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    u32 jump0_instr_index = encoder->instrs.len;
    instr = (Instr) {
      InstrKindJumpIfNot,
      {
        .jump_if_not = {
          cond_index,
          0,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    Segments segments;
    segments.len = 1;
    segments.cap = segments.len;
    segments.items = arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));
    segments.items[0].offset = 0;
    segments.items[0].size = 8;

    instr = (Instr) {
      InstrKindCopyFromRefFixed,
      {
        .copy_from_ref_fixed = {
          new_index,
          temp_index,
          segments,
          ValueKindUnsigned,
          8,
          true,
          false,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindBinOp,
      {
        .bin_op = {
          cond_index,
          new_index,
          zero_index,
          BinOpKindNeInt,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    u32 jump1_instr_index = encoder->instrs.len;
    instr = (Instr) {
      InstrKindJumpIfNot,
      {
        .jump_if_not = {
          cond_index,
          0,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    segments.len = 1;
    segments.cap = segments.len;
    segments.items = arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));
    segments.items[0].offset = 0;
    segments.items[0].size = 8;

    instr = (Instr) {
      InstrKindCopyFromRefFixed,
      {
        .copy_from_ref_fixed = {
          temp_index,
          temp_index,
          segments,
          ValueKindUnsigned,
          8,
          true,
          false,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindJump,
      {
        .jump = {
          cond_instr_index,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    encoder->instrs.items[jump0_instr_index].as.jump_if_not.target = encoder->instrs.len;
    encoder->instrs.items[jump1_instr_index].as.jump_if_not.target = encoder->instrs.len;

    Var size_var = {
      {},
      arena_alloc(encoder->arena, sizeof(Type)),
    };
    size_var.type->kind = TypeKindInt;
    DA_APPEND(*encoder->vars, size_var);
    u32 size_index = define_var(encoder);

    u32 element_size = get_type_size(encoder->vars->items[1].type);
    instr = (Instr) {
      InstrKindStore,
      {
        .store = {
          size_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = 8 + element_size,
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    u32 size = get_type_size(built_in->_return->type);
    ValueKind kind = get_type_value_kind(built_in->_return->type);

    Indices arg_indices;
    arg_indices.len = 1;
    arg_indices.cap = arg_indices.len;
    arg_indices.items = arena_alloc(encoder->arena, arg_indices.cap * sizeof(u32));
    arg_indices.items[0] = size_index;

    instr = (Instr) {
      InstrKindCallAssign,
      {
        .call_assign = {
          new_index,
          size,
          kind,
          STR_LIT("ether_alloc"),
          arg_indices,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    try_gen_rc_inc(encoder, new_index);

    segments.len = 2;
    segments.cap = segments.len;
    segments.items = arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));
    segments.items[0].offset = 0;
    segments.items[0].size = 8;
    segments.items[1].offset = 8;
    segments.items[1].size = element_size;

    instr = (Instr) {
      InstrKindCopyToRefFixed,
      {
        .copy_to_ref_fixed = {
          new_index,
          segments,
          1,
          true,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindBinOp,
      {
        .bin_op = {
          cond_index,
          temp_index,
          zero_index,
          BinOpKindNeInt,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    u32 jump2_instr_index = encoder->instrs.len;
    instr = (Instr) {
      InstrKindJumpIfNot,
      {
        .jump_if_not = {
          cond_index,
          0,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    segments.len = 1;
    segments.cap = segments.len;
    segments.items = arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));
    segments.items[0].offset = 0;
    segments.items[0].size = 8;

    instr = (Instr) {
      InstrKindCopyToRefFixed,
      {
        .copy_to_ref_fixed = {
          temp_index,
          segments,
          new_index,
          true,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindRetVal,
      {
        .ret_val = {
          0,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    encoder->instrs.items[jump2_instr_index].as.jump_if_not.target = encoder->instrs.len;

    instr = (Instr) {
      InstrKindRetVal,
      {
        .ret_val = {
          new_index,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);
  } else if (begins_with(built_in->name, STR_LIT("ether_rc_dec_1"))) {
    Var ctx_var = {
      {},
      arena_alloc(encoder->arena, sizeof(Type)),
    };
    ctx_var.type->kind = TypeKindInt;
    DA_APPEND(*encoder->vars, ctx_var);
    u32 ctx_index = define_var(encoder);

    Segments segments;
    segments.len = 2;
    segments.cap = segments.len;
    segments.items = arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));
    segments.items[0].offset = 0;
    segments.items[0].size = 8;
    segments.items[1].offset = 8;
    segments.items[1].size = 8;

    Instr instr = {
      InstrKindCopyFromRefFixed,
      {
        .copy_from_ref_fixed = {
          ctx_index,
          0,
          segments,
          ValueKindUnsigned,
          8,
          false,
          false,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    Var refs_var = {
      {},
      arena_alloc(encoder->arena, sizeof(Type)),
    };
    refs_var.type->kind = TypeKindInt;
    DA_APPEND(*encoder->vars, refs_var);
    u32 refs_index = define_var(encoder);

    segments.len = 1;
    segments.cap = segments.len;
    segments.items = arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));
    segments.items[0].offset = -8;
    segments.items[0].size = 8;

    instr = (Instr) {
      InstrKindCopyFromRefFixed,
      {
        .copy_from_ref_fixed = {
          refs_index,
          ctx_index,
          segments,
          ValueKindUnsigned,
          8,
          true,
          false,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    Var one_var = {
      {},
      arena_alloc(encoder->arena, sizeof(Type)),
    };
    one_var.type->kind = TypeKindInt;
    DA_APPEND(*encoder->vars, one_var);
    u32 one_index = define_var(encoder);

    instr = (Instr) {
      InstrKindStore,
      {
        .store = {
          one_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = 1,
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindBinOp,
      {
        .bin_op = {
          refs_index,
          refs_index,
          one_index,
          BinOpKindSubInt,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    Var cond_var = {
      {},
      arena_alloc(encoder->arena, sizeof(Type)),
    };
    cond_var.type->kind = TypeKindInt;
    DA_APPEND(*encoder->vars, cond_var);
    u32 cond_index = define_var(encoder);

    Var zero_var = {
      {},
      arena_alloc(encoder->arena, sizeof(Type)),
    };
    zero_var.type->kind = TypeKindInt;
    DA_APPEND(*encoder->vars, zero_var);
    u32 zero_index = define_var(encoder);

    instr = (Instr) {
      InstrKindStore,
      {
        .store = {
          zero_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = 0,
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindBinOp,
      {
        .bin_op = {
          cond_index,
          refs_index,
          zero_index,
          BinOpKindEqInt,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    u32 jump_instr_index = encoder->instrs.len;
    instr = (Instr) {
      InstrKindJumpIfNot,
      {
        .jump_if_not = {
          cond_index,
          0,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    Vars *captured_vars = &encoder->funcs->items[built_in->args.items[0]->type->func_index].captured_vars;
    for (u32 i = 0; i < captured_vars->len; ++i) {
      if (!type_is_rc(captured_vars->items[i].type))
        continue;

      Var captured_var = {
        {},
        captured_vars->items[i].type,
      };
      DA_APPEND(*encoder->vars, captured_var);
      u32 captured_index = define_var(encoder);

      segments.len = i + 2;
      segments.cap = segments.len;
      segments.items = arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));

      segments.items[0].offset = 0;
      segments.items[0].size = 8;

      i32 offset = 0;
      for (u32 j = 0; j <= i; ++j) {
        u32 size = get_type_size(captured_vars->items[j].type);
        segments.items[j + 1].offset = offset;
        segments.items[j + 1].size = size;
        offset += size;
      }

      Instr instr = {
        InstrKindCopyFromRefFixed,
        {
          .copy_from_ref_fixed = {
            captured_index,
            ctx_index,
            segments,
            ValueKindUnsigned,
            8,
            true,
            false,
          },
        },
      };
      DA_APPEND(encoder->instrs, instr);

      try_gen_rc_dec(encoder, captured_index);
    }

    Var size_var = {
      {},
      arena_alloc(encoder->arena, sizeof(Type)),
    };
    size_var.type->kind = TypeKindInt;
    DA_APPEND(*encoder->vars, size_var);
    u32 size_index = define_var(encoder);

    u32 captured_size = 0;
    for (u32 i = 0; i < captured_vars->len; ++i)
      captured_size += get_type_size(captured_vars->items[i].type);

    instr = (Instr) {
      InstrKindStore,
      {
        .store = {
          size_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = captured_size,
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    Indices arg_indices;
    arg_indices.len = 2;
    arg_indices.cap = arg_indices.len;
    arg_indices.items = arena_alloc(encoder->arena, arg_indices.cap * sizeof(u32));
    arg_indices.items[0] = ctx_index;
    arg_indices.items[1] = size_index;

    instr = (Instr) {
      InstrKindCall,
      {
        .call = {
          STR_LIT("ether_free"),
          arg_indices,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindRet,
      {},
    };
    DA_APPEND(encoder->instrs, instr);

    encoder->instrs.items[jump_instr_index].as.jump_if_not.target = encoder->instrs.len;

    segments.len = 1;
    segments.cap = segments.len;
    segments.items = arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));
    segments.items[0].offset = -8;
    segments.items[0].size = 8;

    instr = (Instr) {
      InstrKindCopyToRefFixed,
      {
        .copy_to_ref_fixed = {
          ctx_index,
          segments,
          refs_index,
          true,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);
  } else if (begins_with(built_in->name, STR_LIT("ether_rc_dec_5"))) {
    Var cond_var = {
      {},
      arena_alloc(encoder->arena, sizeof(Type)),
    };
    cond_var.type->kind = TypeKindInt;
    DA_APPEND(*encoder->vars, cond_var);
    u32 cond_index = define_var(encoder);

    Var temp_var = {
      {},
      arena_alloc(encoder->arena, sizeof(Type)),
    };
    temp_var.type->kind = TypeKindInt;
    DA_APPEND(*encoder->vars, temp_var);
    u32 temp_index = define_var(encoder);

    Instr instr = {
      InstrKindStore,
      {
        .store = {
          temp_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = 0,
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindBinOp,
      {
        .bin_op = {
          cond_index,
          0,
          temp_index,
          BinOpKindNeInt,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    u32 jump0_instr_index = encoder->instrs.len;
    instr = (Instr) {
      InstrKindJumpIfNot,
      {
        .jump_if_not = {
          cond_index,
          0,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    Var one_var = {
      {},
      arena_alloc(encoder->arena, sizeof(Type)),
    };
    one_var.type->kind = TypeKindInt;
    DA_APPEND(*encoder->vars, one_var);
    u32 one_index = define_var(encoder);

    instr = (Instr) {
      InstrKindStore,
      {
        .store = {
          one_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = 1,
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    Segments segments;
    segments.len = 1;
    segments.cap = segments.len;
    segments.items = arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));
    segments.items[0].offset = -8;
    segments.items[0].size = 8;

    instr = (Instr) {
      InstrKindCopyFromRefFixed,
      {
        .copy_from_ref_fixed = {
          temp_index,
          0,
          segments,
          ValueKindUnsigned,
          8,
          true,
          false,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindBinOp,
      {
        .bin_op = {
          temp_index,
          temp_index,
          one_index,
          BinOpKindSubInt,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    Var zero_var = {
      {},
      arena_alloc(encoder->arena, sizeof(Type)),
    };
    zero_var.type->kind = TypeKindInt;
    DA_APPEND(*encoder->vars, zero_var);
    u32 zero_index = define_var(encoder);

    instr = (Instr) {
      InstrKindStore,
      {
        .store = {
          zero_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = 0,
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindBinOp,
      {
        .bin_op = {
          cond_index,
          temp_index,
          zero_index,
          BinOpKindEqInt,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    u32 jump1_instr_index = encoder->instrs.len;
    instr = (Instr) {
      InstrKindJumpIfNot,
      {
        .jump_if_not = {
          cond_index,
          0,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    StringBuilder sb = {0};
    sb_push_str(&sb, STR_LIT("ether_free_"));
    sb_push_type_hash(&sb, encoder->funcs, built_in->args.items[0]->type);

    Str name;
    name.len = sb.len;
    name.ptr = arena_alloc(encoder->arena, name.len);
    memcpy(name.ptr, sb.buffer, name.len);
    free(sb.buffer);

    Indices arg_indices;
    arg_indices.len = 1;
    arg_indices.cap = arg_indices.len;
    arg_indices.items = arena_alloc(encoder->arena, arg_indices.cap * sizeof(u32));
    arg_indices.items[0] = 0;

    instr = (Instr) {
      InstrKindCall,
      {
        .call = {
          name,
          arg_indices,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) { InstrKindRet, {} };
    DA_APPEND(encoder->instrs, instr);

    encoder->instrs.items[jump1_instr_index].as.jump_if_not.target = encoder->instrs.len;

    segments.len = 1;
    segments.cap = segments.len;
    segments.items = arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));
    segments.items[0].offset = -8;
    segments.items[0].size = 8;

    instr = (Instr) {
      InstrKindCopyToRefFixed,
      {
        .copy_to_ref_fixed = {
          0,
          segments,
          temp_index,
          true,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    encoder->instrs.items[jump0_instr_index].as.jump_if_not.target = encoder->instrs.len;

    instr = (Instr) { InstrKindRet, {} };
    DA_APPEND(encoder->instrs, instr);
  } else if (begins_with(built_in->name, STR_LIT("ether_get_value_len_as_str_5"))) {
    Var dest_var = {
      {},
      built_in->_return->type,
    };
    DA_APPEND(*encoder->vars, dest_var);
    u32 dest_index = define_var(encoder);

    u32 size = get_type_size(built_in->_return->type);
    ValueKind kind = get_type_value_kind(built_in->_return->type);

    Indices arg_indices;
    arg_indices.len = 1;
    arg_indices.cap = arg_indices.len;
    arg_indices.items = arena_alloc(encoder->arena, arg_indices.cap * sizeof(u32));
    arg_indices.items[0] = 0;

    Instr instr = {
      InstrKindCallAssign,
      {
        .call_assign = {
          dest_index,
          size,
          kind,
          STR_LIT("len"),
          arg_indices,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    Var one_var = {
      {},
      arena_alloc(encoder->arena, sizeof(Type)),
    };
    one_var.type->kind = TypeKindInt;
    DA_APPEND(*encoder->vars, one_var);
    u32 one_index = define_var(encoder);

    instr = (Instr) {
      InstrKindStore,
      {
        .store = {
          one_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = 1,
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindBinOp,
      {
        .bin_op = {
          dest_index,
          dest_index,
          one_index,
          BinOpKindAddInt,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    Var zero_var = {
      {},
      arena_alloc(encoder->arena, sizeof(Type)),
    };
    zero_var.type->kind = TypeKindInt;
    DA_APPEND(*encoder->vars, zero_var);
    u32 zero_index = define_var(encoder);

    instr = (Instr) {
      InstrKindStore,
      {
        .store = {
          zero_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = 0,
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    Var cond_var = {
      {},
      built_in->_return->type,
    };
    DA_APPEND(*encoder->vars, cond_var);
    u32 cond_index = define_var(encoder);

    instr = (Instr) {
      InstrKindBinOp,
      {
        .bin_op = {
          cond_index,
          0,
          zero_index,
          BinOpKindEqInt,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    u32 jump0_instr_index = encoder->instrs.len;
    instr = (Instr) {
      InstrKindJumpIfNot,
      {
        .jump_if_not = {
          cond_index,
          0,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindBinOp,
      {
        .bin_op = {
          dest_index,
          dest_index,
          one_index,
          BinOpKindAddInt,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    encoder->instrs.items[jump0_instr_index].as.jump_if_not.target = encoder->instrs.len;

    u32 cond_instr_index = encoder->instrs.len;

    instr = (Instr) {
      InstrKindBinOp,
      {
        .bin_op = {
          cond_index,
          0,
          zero_index,
          BinOpKindNeInt,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    u32 jump1_instr_index = encoder->instrs.len;
    instr = (Instr) {
      InstrKindJumpIfNot,
      {
        .jump_if_not = {
          cond_index,
          0,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    StringBuilder sb = {0};
    sb_push_str(&sb, STR_LIT("ether_get_value_len_as_str_"));
    sb_push_type_hash(&sb, NULL, built_in->args.items[0]->type->element_type);
    if (built_in->args.items[0]->type->element_type->kind == TypeKindStr)
      sb_push(&sb, "_quoted");

    Str name;
    name.len = sb.len;
    name.ptr = arena_alloc(encoder->arena, name.len);
    memcpy(name.ptr, sb.buffer, name.len);
    free(sb.buffer);

    Var element_var = {
      {},
      built_in->args.items[0]->type->element_type,
    };
    DA_APPEND(*encoder->vars, element_var);
    u32 element_index = define_var(encoder);

    u32 element_size = get_type_size(built_in->args.items[0]->type->element_type);

    Segments segments;
    segments.len = 2;
    segments.cap = segments.len;
    segments.items = arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));
    segments.items[0].offset = 0;
    segments.items[0].size = 8;
    segments.items[1].offset = 8;
    segments.items[1].size = element_size;

    instr = (Instr) {
      InstrKindCopyFromRefFixed,
      {
        .copy_from_ref_fixed = {
          element_index,
          0,
          segments,
          ValueKindUnsigned,
          8,
          true,
          false,
        },
      },
    };

    DA_APPEND(encoder->instrs, instr);
    arg_indices.items = arena_alloc(encoder->arena, arg_indices.cap * sizeof(u32));
    arg_indices.items[0] = element_index;

    instr = (Instr) {
      InstrKindCallAssign,
      {
        .call_assign = {
          cond_index,
          size,
          kind,
          name,
          arg_indices,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindBinOp,
      {
        .bin_op = {
          dest_index,
          dest_index,
          cond_index,
          BinOpKindAddInt,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    segments.len = 1;
    segments.cap = segments.len;
    segments.items = arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));
    segments.items[0].offset = 0;
    segments.items[0].size = 8;

    instr = (Instr) {
      InstrKindCopyFromRefFixed,
      {
        .copy_from_ref_fixed = {
          0,
          0,
          segments,
          ValueKindUnsigned,
          8,
          true,
          false,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindJump,
      {
        .jump = {
          cond_instr_index,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    encoder->instrs.items[jump1_instr_index].as.jump_if_not.target = encoder->instrs.len;

    instr = (Instr) {
      InstrKindRetVal,
      {
        .ret_val = {
          dest_index,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);
  } else if (begins_with(built_in->name, STR_LIT("ether_value_to_str_5"))) {
    Indices arg_indices;
    arg_indices.len = 1;
    arg_indices.cap = arg_indices.len;
    arg_indices.items = arena_alloc(encoder->arena, arg_indices.cap * sizeof(u32));
    arg_indices.items[0] = 0;

    Var dest_var = {
      {},
      built_in->_return->type,
    };
    DA_APPEND(*encoder->vars, dest_var);
    u32 dest_index = define_var(encoder);

    Instr instr = {
      InstrKindStore,
      {
        .store = {
          dest_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = 1,
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    Var zero_var = {
      {},
      arena_alloc(encoder->arena, sizeof(Type)),
    };
    zero_var.type->kind = TypeKindInt;
    DA_APPEND(*encoder->vars, zero_var);
    u32 zero_index = define_var(encoder);

    instr = (Instr) {
      InstrKindStore,
      {
        .store = {
          zero_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = 0,
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    Var one_var = {
      {},
      arena_alloc(encoder->arena, sizeof(Type)),
    };
    one_var.type->kind = TypeKindInt;
    DA_APPEND(*encoder->vars, one_var);
    u32 one_index = define_var(encoder);

    instr = (Instr) {
      InstrKindStore,
      {
        .store = {
          one_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = 1,
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    Var char_var = {
      {},
      NULL,
    };
    DA_APPEND(*encoder->vars, char_var);
    u32 char_index = encoder->vars_defined++;

    Segments segments;
    segments.len = 1;
    segments.cap = segments.len;
    segments.items = arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));
    segments.items[0].offset = 0;
    segments.items[0].size = 1;

    instr = (Instr) {
      InstrKindAlloc,
      {
        .alloc = {
          char_index,
          segments,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindStore,
      {
        .store = {
          char_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = '[',
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindCopyToRef,
      {
        .copy_to_ref = {
          0,
          1,
          char_index,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindStore,
      {
        .store = {
          char_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = ' ',
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    Var offset_var = {
      {},
      built_in->_return->type,
    };
    DA_APPEND(*encoder->vars, offset_var);
    u32 offset_index = define_var(encoder);

    instr = (Instr) {
      InstrKindBinOp,
      {
        .bin_op = {
          offset_index,
          1,
          dest_index,
          BinOpKindAddInt,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    Var cond_var = {
      {},
      built_in->_return->type,
    };
    DA_APPEND(*encoder->vars, cond_var);
    u32 cond_index = define_var(encoder);

    instr = (Instr) {
      InstrKindBinOp,
      {
        .bin_op = {
          cond_index,
          2,
          zero_index,
          BinOpKindEqInt,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    u32 jump0_instr_index = encoder->instrs.len;
    instr = (Instr) {
      InstrKindJumpIfNot,
      {
        .jump_if_not = {
          cond_index,
          0,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindStore,
      {
        .store = {
          char_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = ']',
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindCopyToRef,
      {
        .copy_to_ref = {
          0,
          offset_index,
          char_index,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    Var two_var = {
      {},
      arena_alloc(encoder->arena, sizeof(Type)),
    };
    two_var.type->kind = TypeKindInt;
    DA_APPEND(*encoder->vars, two_var);
    u32 two_index = define_var(encoder);

    instr = (Instr) {
      InstrKindStore,
      {
        .store = {
          two_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = 2,
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindRetVal,
      {
        .ret_val = {
          two_index,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    encoder->instrs.items[jump0_instr_index].as.jump_if_not.target = encoder->instrs.len;

    u32 cond_instr_index = encoder->instrs.len;

    instr = (Instr) {
      InstrKindBinOp,
      {
        .bin_op = {
          cond_index,
          2,
          zero_index,
          BinOpKindNeInt,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    u32 jump1_instr_index = encoder->instrs.len;
    instr = (Instr) {
      InstrKindJumpIfNot,
      {
        .jump_if_not = {
          cond_index,
          0,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    StringBuilder sb = {0};
    sb_push_str(&sb, STR_LIT("ether_value_to_str_"));
    sb_push_type_hash(&sb, NULL, built_in->args.items[2]->type->element_type);
    if (built_in->args.items[2]->type->element_type->kind == TypeKindStr)
      sb_push(&sb, "_quoted");

    Str name;
    name.len = sb.len;
    name.ptr = arena_alloc(encoder->arena, name.len);
    memcpy(name.ptr, sb.buffer, name.len);
    free(sb.buffer);

    Var element_var = {
      {},
      built_in->args.items[2]->type->element_type,
    };
    DA_APPEND(*encoder->vars, element_var);
    u32 element_index = define_var(encoder);

    u32 element_size = get_type_size(built_in->args.items[2]->type->element_type);

    segments.len = 2;
    segments.cap = segments.len;
    segments.items = arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));
    segments.items[0].offset = 0;
    segments.items[0].size = 8;
    segments.items[1].offset = 8;
    segments.items[1].size = element_size;

    instr = (Instr) {
      InstrKindCopyFromRefFixed,
      {
        .copy_from_ref_fixed = {
          element_index,
          2,
          segments,
          ValueKindUnsigned,
          8,
          true,
          false,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    u32 size = get_type_size(built_in->_return->type);
    ValueKind kind = get_type_value_kind(built_in->_return->type);

    DA_APPEND(encoder->instrs, instr);
    arg_indices.len = 3;
    arg_indices.cap = arg_indices.len;
    arg_indices.items = arena_alloc(encoder->arena, arg_indices.cap * sizeof(u32));
    arg_indices.items[0] = 0;
    arg_indices.items[1] = offset_index;
    arg_indices.items[2] = element_index;

    instr = (Instr) {
      InstrKindCallAssign,
      {
        .call_assign = {
          cond_index,
          size,
          kind,
          name,
          arg_indices,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindBinOp,
      {
        .bin_op = {
          dest_index,
          dest_index,
          cond_index,
          BinOpKindAddInt,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindBinOp,
      {
        .bin_op = {
          offset_index,
          1,
          dest_index,
          BinOpKindAddInt,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindCopyToRef,
      {
        .copy_to_ref = {
          0,
          offset_index,
          char_index,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindBinOp,
      {
        .bin_op = {
          dest_index,
          dest_index,
          one_index,
          BinOpKindAddInt,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindBinOp,
      {
        .bin_op = {
          offset_index,
          1,
          dest_index,
          BinOpKindAddInt,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    segments.len = 1;
    segments.cap = segments.len;
    segments.items = arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));
    segments.items[0].offset = 0;
    segments.items[0].size = 8;

    instr = (Instr) {
      InstrKindCopyFromRefFixed,
      {
        .copy_from_ref_fixed = {
          2,
          2,
          segments,
          ValueKindUnsigned,
          8,
          true,
          false,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindJump,
      {
        .jump = {
          cond_instr_index,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    encoder->instrs.items[jump1_instr_index].as.jump_if_not.target = encoder->instrs.len;

    instr = (Instr) {
      InstrKindBinOp,
      {
        .bin_op = {
          offset_index,
          offset_index,
          one_index,
          BinOpKindSubInt,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindStore,
      {
        .store = {
          char_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = ']',
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindCopyToRef,
      {
        .copy_to_ref = {
          0,
          offset_index,
          char_index,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    instr = (Instr) {
      InstrKindRetVal,
      {
        .ret_val = {
          dest_index,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);
  } else if (begins_with(built_in->name, STR_LIT("ether_free_5"))) {
    Var next_var = {
      {},
      built_in->args.items[0]->type,
    };
    DA_APPEND(*encoder->vars, next_var);
    u32 next_index = define_var(encoder);

    Segments segments;
    segments.len = 1;
    segments.cap = segments.len;
    segments.items = arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));
    segments.items[0].offset = 0;
    segments.items[0].size = 8;

    Instr instr = {
      InstrKindCopyFromRefFixed,
      {
        .copy_from_ref_fixed = {
          next_index,
          0,
          segments,
          ValueKindUnsigned,
          8,
          true,
          false,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    try_gen_rc_dec(encoder, next_index);

    Type *element_type = built_in->args.items[0]->type->element_type;
    u32 element_size = get_type_size(element_type);

    if (type_is_rc(element_type)) {
      Var element_var = {
        {},
        element_type,
      };
      DA_APPEND(*encoder->vars, element_var);
      u32 element_index = define_var(encoder);

      segments.len = 2;
      segments.cap = segments.len;
      segments.items = arena_alloc(encoder->arena, segments.cap * sizeof(AlignedSegment));
      segments.items[0].offset = 0;
      segments.items[0].size = 8;
      segments.items[1].offset = 8;
      segments.items[1].size = element_size;

      Instr instr = {
        InstrKindCopyFromRefFixed,
        {
          .copy_from_ref_fixed = {
            element_index,
            0,
            segments,
            get_type_value_kind(element_type),
            get_type_size(element_type),
            true,
            false,
          },
        },
      };
      DA_APPEND(encoder->instrs, instr);

      try_gen_rc_dec(encoder, element_index);
    }

    StringBuilder sb = {0};
    sb_push_str(&sb, STR_LIT("ether_free_"));
    sb_push_type_hash(&sb, encoder->funcs, built_in->args.items[0]->type->element_type);

    Var size_var = {
      {},
      built_in->args.items[0]->type,
    };
    DA_APPEND(*encoder->vars, size_var);
    u32 size_index = define_var(encoder);

    instr = (Instr) {
      InstrKindStore,
      {
        .store = {
          size_index,
          {
            ValueKindUnsigned,
            {
              ._unsigned = element_size,
            },
          },
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);

    Str name;
    name.len = sb.len;
    name.ptr = arena_alloc(encoder->arena, name.len);
    memcpy(name.ptr, sb.buffer, name.len);
    free(sb.buffer);

    Indices arg_indices;
    arg_indices.len = 2;
    arg_indices.cap = arg_indices.len;
    arg_indices.items = arena_alloc(encoder->arena, arg_indices.cap * sizeof(u32));
    arg_indices.items[0] = 0;
    arg_indices.items[1] = size_index;

    instr = (Instr) {
      InstrKindCall,
      {
        .call = {
          STR_LIT("ether_free"),
          arg_indices,
        },
      },
    };
    DA_APPEND(encoder->instrs, instr);
  } else {
    ERROR("Cannot generate built-in "STR_FMT"\n", STR_ARG(built_in->name));
    exit(1);
  }
}

void encode_ast_as_evm_ir(FILE *stream, Arena *arena, Funcs *funcs) {
  Encoder encoder = {0};
  encoder.stream = stream;
  encoder.arena = arena;
  encoder.funcs = funcs;

  Da(Instrs) instrss = {0};
  Da(Instrs) last_instrss = {0};
  u32 funcs_encoded = 0;

  for (u32 i = 0; i < funcs->len; ++i) {
    Func *func = funcs->items + i;

    if (!func->is_checked)
      continue;

    ++funcs_encoded;

    encoder.instrs = (Instrs) {0};
    encoder.vars = &func->vars;
    // + 1 is for context that contains captured values
    encoder.vars_defined = func->captured_vars.len + func->expr->args.len + 1;

    for (u32 j = 0; j < func->captured_vars.len; ++j)
      get_captured_var(&encoder, j, j);

    Exprs *body = &funcs->items[i].expr->body;
    bool is_result_zero_sized = get_type_size(func->type->return_type) == 0;

    encode_block(&encoder, body, (u32) -1, !is_result_zero_sized);

    if (body->len > 0 && body->items[body->len - 1]->kind != ExprKindRet) {
      if (is_result_zero_sized) {
        encode_expr(&encoder, body->items[body->len - 1], (u32) -1);
      } else {
        u32 index = define_var(&encoder);
        encode_expr(&encoder, body->items[body->len - 1], index);

        Instr instr = {
          InstrKindRetVal,
          {
            .ret_val = { index },
          },
        };
        DA_APPEND(encoder.instrs, instr);
      }
    }

    DA_APPEND(instrss, encoder.instrs);
    encoder.instrs = (Instrs) {0};

    for (u32 j = encoder.vars->len; j > func->expr->args.len; --j)
      if (encoder.vars->items[j - 1].name.len > 0)
        try_gen_rc_dec(&encoder, j - 1);

    DA_APPEND(last_instrss, encoder.instrs);
  }

  for (u32 i = 0; i < encoder.built_ins_to_gen.len; ++i) {
    BuiltInToGen *built_in = encoder.built_ins_to_gen.items + i;

    encoder.instrs = (Instrs) {0};
    encoder.vars = &built_in->vars;
    encoder.vars_defined = built_in->args.len;

    for (u32 j = 0; j < built_in->args.len; ++j)
      DA_APPEND(*encoder.vars, *built_in->args.items[j]);

    encode_built_in_to_gen(&encoder, built_in);

    DA_APPEND(instrss, encoder.instrs);
    DA_APPEND(last_instrss, (Instrs) {0});
  }

  fwrite(&instrss.len, sizeof(instrss.len), 1, stream);

  for (u32 i = 0; i < encoder.built_ins_to_gen.len; ++i) {
    BuiltInToGen *built_in = encoder.built_ins_to_gen.items + i;

    encode_str(stream, built_in->name);

    fwrite(&built_in->args.len, sizeof(built_in->args.len), 1, stream);
    for (u32 j = 0; j < built_in->args.len; ++j)
      encode_type(stream, built_in->args.items[j]->type);

    if (built_in->_return) {
      encode_type(stream, built_in->_return->type);
    } else {
      Type type = { TypeKindUnit, {} };
      encode_type(stream, &type);
    }

    Instrs *instrs = instrss.items + funcs_encoded + i;
    fwrite(&instrs->len, sizeof(instrs->len), 1, stream);
    for (u32 j = 0; j < instrs->len; ++j)
      encode_instr(encoder.stream, instrs->items + j);

    u32 zero = 0;
    fwrite(&zero, sizeof(zero), 1, stream);
  }

  funcs_encoded = 0;
  for (u32 i = 0; i < funcs->len; ++i) {
    Func *func = funcs->items + i;

    if (!func->is_checked)
      continue;

    if (str_eq(func->expr->name, STR_LIT("main")) || func->is_lambda) {
      encode_str(stream, func->expr->name);
    } else {
      Str name = mangle_func_name(func);
      encode_str(stream, name);
      free(name.ptr);
    }

    u32 args_len = func->expr->args.len + 1;
    fwrite(&args_len, sizeof(args_len), 1, stream);
    Type ctx_arg_type = { TypeKindInt, {} };
    encode_type(stream, &ctx_arg_type);
    for (u32 j = 0; j < func->expr->args.len; ++j)
      encode_type(stream, func->type->arg_types.items[j]);
    encode_type(stream, func->type->return_type);

    Instrs *instrs = instrss.items + funcs_encoded;
    fwrite(&instrs->len, sizeof(instrs->len), 1, stream);
    for (u32 j = 0; j < instrs->len; ++j)
      encode_instr(encoder.stream, instrs->items + j);

    instrs = last_instrss.items + funcs_encoded;
    fwrite(&instrs->len, sizeof(instrs->len), 1, stream);
    for (u32 j = 0; j < instrs->len; ++j)
      encode_instr(encoder.stream, instrs->items + j);

    ++funcs_encoded;
  }

  fwrite(&encoder.data.len, sizeof(encoder.data.len), 1, stream);
  for (u32 i = 0; i < encoder.data.len; ++i) {
    DataEntry *entry = encoder.data.items + i;

    u32 len = entry->len + 6; // Size, null-terminator and static marker
    u8 zero = 0;
    u8 one = 1;
    fwrite(&len, sizeof(len), 1, stream);
    fwrite(&entry->len, sizeof(entry->len), 1, stream);
    fwrite(entry->data, 1, entry->len, stream);
    fwrite(&zero, 1, 1, stream);
    fwrite(&one, 1, 1, stream);
  }

  fwrite(&built_ins_len, sizeof(built_ins_len), 1, stream);
  for (u32 i = 0; i < built_ins_len; ++i)
    encode_str(encoder.stream, built_ins[i].name);

  for (u32 i = 0; i < instrss.len; ++i)
    if (instrss.items[i].items)
      free(instrss.items[i].items);
  if (instrss.items)
    free(instrss.items);

  for (u32 i = 0; i < last_instrss.len; ++i)
    if (last_instrss.items[i].items)
      free(last_instrss.items[i].items);
  if (last_instrss.items)
    free(last_instrss.items);

  if (encoder.data.items)
    free(encoder.data.items);

  if (encoder.built_ins_to_gen.items)
    free(encoder.built_ins_to_gen.items);
}
