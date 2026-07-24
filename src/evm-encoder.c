#include "evm-encoder.h"
#include "evm/ir.h"

typedef struct {
  FILE   *stream;
  Instrs  instrs;
  Vars   *vars;
  u32     vars_defined;
} Encoder;

static void encode_str(FILE *stream, Str str) {
  fwrite(&str.len, sizeof(str.len), 1, stream);
  fwrite(str.ptr, 1, str.len, stream);
}

static void encode_type(FILE *stream, Type *type) {
  (void) type;

  u32 size = 8;
  u8 kind = ValueKindSigned;
  fwrite(&size, sizeof(size), 1, stream);
  fwrite(&kind, sizeof(kind), 1, stream);
}

static u32 define_var(Encoder *encoder) {
  Segments segments = {0};
  AlignedSegment segment = {
    0,
    // get_type_size(&encoder->vars->items[encoder->vars_defined].type),
    8,
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
  }

  return 0;
}

// static ValueKind get_type_value_kind(Type *type) {
//   switch (type->kind) {
//   case TypeKindUnit: return ValueKindUnsigned;
//   case TypeKindFunc: return ValueKindUnsigned;
//   case TypeKindInt:  return ValueKindSigned;
//   case TypeKindBool: return ValueKindUnsigned;
//   case TypeKindStr:  return ValueKindUnsigned;
//   }

//   return 0;
// }

static void encode_block(Encoder *encoder, Exprs *block,
                         u32 dest_index, bool last_is_return);

static void encode_expr(Encoder *encoder, Expr *expr, u32 dest_index) {
  switch (expr->kind) {
  case ExprKindStr: {
    fprintf(stderr, "Expr not yet implemented: %u\n", expr->kind);
    exit(1);
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
      Instr instr = {
        InstrKindRefProc,
        {
          .ref_proc = {
            dest_index,
            expr->as.ident.name,
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
      DA_APPEND(arg_indices, arg_index);
    }

    Instr instr;
    if (opt) {
      if (dest_index == (u32) -1) {
        instr = (Instr) {
          InstrKindCall,
          {
            .call = {
              expr->as.func_call.func->as.ident.name,
              arg_indices,
            },
          },
        };
      } else {
        instr = (Instr) {
          InstrKindCallAssign,
          {
            .call_assign = {
              dest_index,
              expr->as.func_call.func->as.ident.name,
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
        // Var *var = encoder->vars->items + index;
        // u32 size = get_type_size(var->type.return_type);
        // ValueKind kind = get_type_value_kind(var->type.return_type);
        u32 size = 8;
        ValueKind kind = ValueKindSigned;

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
    if (dest_index == (u32) -1 || expr->as.bin_op.args.len < 2)
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
  }
}

static void encode_block(Encoder *encoder, Exprs *block,
                         u32 dest_index, bool last_is_return) {
  for (u32 i = 0; i + (last_is_return | (dest_index != (u32) -1)) < block->len; ++i)
    encode_expr(encoder, block->items[i], (u32) -1);

  if (block->len > 0) {
    if (last_is_return) {
      u32 index = define_var(encoder);
      encode_expr(encoder, block->items[block->len - 1], index);

      Instr instr = {
        InstrKindRetVal,
        {
          .ret_val = { index },
        },
      };
      DA_APPEND(encoder->instrs, instr);
    } else if (dest_index != (u32) -1) {
      encode_expr(encoder, block->items[block->len - 1], dest_index);
    }
  }
}

void encode_ast_as_evm_ir(FILE *stream, Funcs *funcs) {
  fwrite(&funcs->len, sizeof(funcs->len), 1, stream);

  Encoder encoder = {0};
  encoder.stream = stream;

  for (u32 i = 0; i < funcs->len; ++i) {
    Func *func = funcs->items + i;

    encoder.instrs.len = 0;
    encoder.vars = &func->vars;
    encoder.vars_defined = func->expr->args.len;

    encode_str(stream, func->expr->name);

    fwrite(&func->expr->args.len, sizeof(func->expr->args.len), 1, stream);

    for (u32 j = 0; j < func->expr->args.len; ++j)
      encode_type(stream, &func->expr->args.items[j].type);
    encode_type(stream, &func->expr->return_type);

    encode_block(&encoder, &func->expr->body, (u32) -1, true);
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
      case InstrKindInlineAsm:
      case InstrKindStoreData:
      case InstrKindConvert:
      case InstrKindCopyToRefFixed:
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
  }

  if (encoder.instrs.items)
    free(encoder.instrs.items);

  u32 data_len = 0;
  fwrite(&data_len, sizeof(data_len), 1, stream);

  u32 imports_len = 0;
  fwrite(&imports_len, sizeof(imports_len), 1, stream);
}
