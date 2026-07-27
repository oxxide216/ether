#include "evm-encoder.h"
#include "evm/ir.h"
#include "built-ins.h"

typedef struct {
  FILE   *stream;
  Arena  *arena;
  Funcs  *funcs;
  Instrs  instrs;
  Data    data;
  Vars   *vars;
  u32     vars_defined;
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
  Segments segments = {0};
  AlignedSegment segment = {
    0,
    get_type_size(encoder->vars->items[encoder->vars_defined].type),
  };
  DA_APPEND(segments, segment);

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

static void sb_push_type_hash(StringBuilder *sb, Type *type) {
  sb_push_u32(sb, type->kind);
  if (type->kind == TypeKindFunc) {
    sb_push_type_hash(sb, type->return_type);
    sb_push_u32(sb, type->arg_types.len);
    for (u32 i = 0; i < type->arg_types.len; ++i)
      sb_push_type_hash(sb, type->arg_types.items[i]);
  }
}

static Str mangle_func_name(Func *func) {
  StringBuilder sb = {0};
  sb_push_str(&sb, func->expr->name);
  sb_push_type_hash(&sb, func->type);
  return sb_to_str(sb);
}

static bool type_is_rc(Type *type) {
  return type->kind == TypeKindStr;
}

static void try_gen_rc_inc(Encoder *encoder, u32 index) {
  if (type_is_rc(encoder->vars->items[index].type)) {
    StringBuilder sb = {0};
    sb_push_str(&sb, STR_LIT("ether_rc_inc_"));
    sb_push_type_hash(&sb, encoder->vars->items[index].type);

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
    sb_push_type_hash(&sb, encoder->vars->items[index].type);

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
    u32 prev_vars_defined = encoder->vars_defined;
    encode_block(encoder, &expr->as.block, dest_index, false);
    encoder->vars_defined = prev_vars_defined;
  } break;

  case ExprKindIdent: {
    if (dest_index == (u32) -1)
      break;

    u32 index = get_var_index(encoder->vars, encoder->vars_defined, expr->as.ident.name);
    if (index == (u32) -1) {
      index = get_func_index(encoder->funcs, expr->as.ident.name);
      Str temp_name = mangle_func_name(encoder->funcs->items + index);
      Str name;
      name.len = temp_name.len;
      name.ptr = arena_alloc(encoder->arena, name.len);
      memcpy(name.ptr, temp_name.ptr, name.len);
      free(temp_name.ptr);
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

  case ExprKindFunc: break;

  case ExprKindFuncCall: {
    Indices arg_indices = {0};

    bool opt = expr->as.func_call.func->kind == ExprKindIdent;
    if (opt)
      opt = get_var_index(encoder->vars, encoder->vars_defined,
                          expr->as.func_call.func->as.ident.name) == (u32) -1;

    u32 index;
    if (!opt) {
      index = define_var(encoder);
      encode_expr(encoder, expr->as.func_call.func, index);
    }

    for (u32 i = 0; i < expr->as.func_call.args.len; ++i) {
      u32 arg_index = define_var(encoder);
      encode_expr(encoder, expr->as.func_call.args.items[i], arg_index);
      try_gen_rc_inc(encoder, arg_index);
      DA_APPEND(arg_indices, arg_index);
    }

    Instr instr;
    if (opt) {
      Str name;
      if (expr->as.func_call.built_in) {
        name = expr->as.func_call.built_in->name;
      } else {
        u32 func_index = get_func_index(encoder->funcs, expr->as.func_call.func->as.ident.name);
        Str temp_name = mangle_func_name(encoder->funcs->items + func_index);
        name.len = temp_name.len;
        name.ptr = arena_alloc(encoder->arena, name.len);
        memcpy(name.ptr, temp_name.ptr, name.len);
        free(temp_name.ptr);
      }
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
    encode_expr(encoder, expr->as.set.value, index);

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

    for (u32 i = 0; i < expr->as.fstr.parts.len; ++i) {
      FStrPart *part = expr->as.fstr.parts.items + i;
      if (!part->is_var)
        start_size += part->str.len;
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
      if (!part->is_var)
        continue;

      u32 var_index = get_var_index(encoder->vars, encoder->vars_defined, part->str);

      Indices arg_indices;
      arg_indices.len = 1;
      arg_indices.cap = arg_indices.len;
      arg_indices.items = arena_alloc(encoder->arena, arg_indices.cap * sizeof(u32));
      arg_indices.items[0] = var_index;

      StringBuilder sb = {0};
      sb_push_str(&sb, STR_LIT("ether_get_value_len_as_str_"));
      sb_push_type_hash(&sb, encoder->vars->items[var_index].type);

      Str name;
      name.len = sb.len;
      name.ptr = arena_alloc(encoder->arena, name.len);
      memcpy(name.ptr, sb.buffer, name.len);
      free(sb.buffer);

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

    Segments segments = {0};
    AlignedSegment segment = { 0, 8 };
    DA_APPEND(segments, segment);

    instr = (Instr) {
      InstrKindCopyToRefFixed,
      {
        .copy_to_ref_fixed = {
          dest_index,
          segments,
          size_index,
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
      if (part->is_var) {
        u32 var_index = get_var_index(encoder->vars, encoder->vars_defined, part->str);

        arg_indices.items[2] = var_index;

        sb_push_str(&sb, STR_LIT("ether_value_to_str_"));
        sb_push_type_hash(&sb, encoder->vars->items[var_index].type);
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
        sb_push_type_hash(&sb, &str_type);
      }

      Str name;
      name.len = sb.len;
      name.ptr = arena_alloc(encoder->arena, name.len);
      memcpy(name.ptr, sb.buffer, name.len);
      free(sb.buffer);

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
  } break;
  }
}

static void encode_block(Encoder *encoder, Exprs *block,
                         u32 dest_index, bool last_is_return) {
  for (u32 i = 0; i + (last_is_return | (dest_index != (u32) -1)) < block->len; ++i)
    encode_expr(encoder, block->items[i], (u32) -1);

  if (block->len > 0) {
    if (last_is_return && block->items[block->len - 1]->kind != ExprKindRet) {
      u32 index = define_var(encoder);
      encode_expr(encoder, block->items[block->len - 1], index);

      Instr instr = {
        InstrKindRetVal,
        {
          .ret_val = { index },
        },
      };
      DA_APPEND(encoder->instrs, instr);
    } else {
      encode_expr(encoder, block->items[block->len - 1], dest_index);
    }
  }
}

void encode_ast_as_evm_ir(FILE *stream, Arena *arena, Funcs *funcs) {
  fwrite(&funcs->len, sizeof(funcs->len), 1, stream);

  Encoder encoder = {0};
  encoder.stream = stream;
  encoder.arena = arena;
  encoder.funcs = funcs;

  for (u32 i = 0; i < funcs->len; ++i) {
    Func *func = funcs->items + i;

    encoder.instrs.len = 0;
    encoder.vars = &func->vars;
    encoder.vars_defined = func->expr->args.len;

    if (str_eq(func->expr->name, STR_LIT("main"))) {
      encode_str(stream, func->expr->name);
    } else {
      Str name = mangle_func_name(func);
      encode_str(stream, name);
      free(name.ptr);
    }

    fwrite(&func->expr->args.len, sizeof(func->expr->args.len), 1, stream);

    for (u32 j = 0; j < func->expr->args.len; ++j)
      encode_type(stream, func->type->arg_types.items[j]);
    encode_type(stream, func->type->return_type);

    encode_block(&encoder, &func->expr->body, (u32) -1, true);

    for (u32 j = encoder.vars->len; j > 0; --j)
      if (encoder.vars->items[j - 1].name.len > 0 ||
          encoder.vars->items[j - 1].is_arg)
        try_gen_rc_dec(&encoder, j - 1);

    fwrite(&encoder.instrs.len, sizeof(encoder.instrs.len), 1, stream);
    for (u32 j = 0; j < encoder.instrs.len; ++j) {
      Instr *instr = encoder.instrs.items + j;

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
        for (u32 k = 0; k < instr->as.call_assign.arg_indices.len; ++k) {
          u32 arg_index = instr->as.call_assign.arg_indices.items[k];
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

      case InstrKindRef:
      case InstrKindCopyToRef:
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
        fprintf(stderr, "Instr not yet implemented: %u\n", instr->kind);
        exit(1);
      }

      case InstrKindCopyToRefFixed: {
        fwrite(&instr->as.copy_to_ref_fixed.dest_index, sizeof(instr->as.copy_to_ref_fixed.dest_index), 1, stream);

        Segments *segments = &instr->as.copy_to_ref_fixed.dest_segments;
        fwrite(&segments->len, sizeof(segments->len), 1, stream);
        for (u32 k = 0; k < segments->len; ++k) {
          fwrite(&segments->items[k].offset, sizeof(segments->items[k].offset), 1, stream);
          fwrite(&segments->items[k].size, sizeof(segments->items[k].size), 1, stream);
        }

        fwrite(&instr->as.copy_to_ref_fixed.src_index, sizeof(instr->as.copy_to_ref_fixed.src_index), 1, stream);
      } break;

      case InstrKindCopyFromRefFixed: {
        fprintf(stderr, "Instr not yet implemented: %u\n", instr->kind);
        exit(1);
      }

      case InstrKindRefProc: {
        fwrite(&instr->as.ref_proc.dest_index, sizeof(instr->as.ref_proc.dest_index), 1, stream);
        encode_str(stream, instr->as.ref_proc.proc_name);
      } break;

      case InstrKindCallRef: {
        fwrite(&instr->as.call_ref.index, sizeof(instr->as.call_ref.index), 1, stream);
        fwrite(&instr->as.call_ref.arg_indices.len, sizeof(instr->as.call_ref.arg_indices.len), 1, stream);
        for (u32 k = 0; k < instr->as.call_ref.arg_indices.len; ++k) {
          u32 arg_index = instr->as.call_ref.arg_indices.items[k];
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
        for (u32 k = 0; k < instr->as.call_ref_assign.arg_indices.len; ++k) {
          u32 arg_index = instr->as.call_ref_assign.arg_indices.items[k];
          fwrite(&arg_index, sizeof(arg_index), 1, stream);
        }
      } break;
      }
    }

    for (u32 j = 0; j < encoder.instrs.len; ++j) {
      Instr *instr = encoder.instrs.items + j;

      if (instr->kind == InstrKindAlloc)
        free(instr->as.alloc.segments.items);
      else if (instr->kind == InstrKindCopyToRefFixed)
        free(instr->as.copy_to_ref_fixed.dest_segments.items);
    }
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

  if (encoder.instrs.items)
    free(encoder.instrs.items);

  if (encoder.data.items)
    free(encoder.data.items);
}
