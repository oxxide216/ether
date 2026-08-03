#include <wchar.h>
#include <ctype.h>

#define SHL_DEFS_LL_ALLOC(size) arena_alloc(arena, size)

#include "parser.h"
#include "io.h"
#include "utils.h"
#include "lexgen/runtime.h"
#define LEXGEN_TRANSITION_TABLE_IMPLEMENTATION
#include "grammar.h"
#include "shl/shl-log.h"

#define MASK(id) (1lu << (id))

typedef enum {
  TokenStatusOk = 0,
  TokenStatusEmpty,
  TokenStatusEOF,
} TokenStatus;

typedef struct {
  u64 id;
  Str lexeme;
  u16 row, col;
} Token;

typedef Da(Token) Tokens;

typedef struct {
  Str              code;
  u32              row, col;
  TransitionTable *table;
  StringBuilder    temp_sb;
  Arena           *arena;
} Lexer;

typedef struct {
  Tokens      tokens;
  Macros     *macros;
  Str         file_path;
  Strs       *included_files;
  Strs       *include_paths;
  CachedASTs *cached_asts;
  Arena      *arena;
  u32         index;
  u32         current_func;
  Da(u32)     label_indices;
} Parser;

static char *token_names[] = {
  "whitespace",
  "new line",
  "comment",
  "fun",
  "let",
  "if",
  "elif",
  "else",
  "macro",
  "set",
  "use",
  "ret",
  "do",
  "`+`",
  "`-`",
  "`*`",
  "`/`",
  "`%`",
  "`=`",
  "`!=`",
  "`<`",
  "`<=`",
  "`>`",
  "`>=`",
  "`(`",
  "`)`",
  "string literal",
  "formatted string literal",
  "`...`",
  "`$`",
  "int",
  "float",
  "bool",
  "identifier",
};

static char escape_char(Str *str, u32 *col) {
  char _char = str->ptr[0];

  switch (_char) {
  case 'n': return '\n';
  case 'r': return '\r';
  case 't': return '\t';
  case 'v': return '\v';
  case 'e': return '\e';
  case 'b': return '\b';
  case '0': return 0;
  case '\\': return '\\';

  case 'x': {
    char result = '\0';

    ++str->ptr;
    --str->len;
    ++*col;

    while (str->len > 0 &&
           ((str->ptr[0] >= '0' && str->ptr[0] <= '9') ||
            (str->ptr[0] >= 'a' && str->ptr[0] <= 'f') ||
            (str->ptr[0] >= 'A' && str->ptr[0] <= 'F'))) {
      result *= 16;

      if (str->ptr[0] >= '0' && str->ptr[0] <= '9')
        result += str->ptr[0] - '0';
      else if (str->ptr[0] >= 'a' && str->ptr[0] <= 'f')
        result += str->ptr[0] - 'a' + 10;
      else if (str->ptr[0] >= 'A' && str->ptr[0] <= 'F')
        result += str->ptr[0] - 'A' + 10;

      ++str->ptr;
      --str->len;
    }

    --str->ptr;
    ++str->len;
    ++*col;

    return result;
  }

  case 'd': {
    char result = '\0';

    ++str->ptr;
    --str->len;
    ++*col;

    while (str->len > 0 && str->ptr[0] >= '0' && str->ptr[0] <= '9') {
      result *= 10;

      if (str->ptr[0] >= '0' && str->ptr[0] <= '9')
        result += str->ptr[0] - '0';

      ++str->ptr;
      --str->len;
      ++*col;
    }

    --str->ptr;
    ++str->len;
    ++*col;

    return result;
  }

  case 'o': {
    char result = '\0';

    ++str->ptr;
    --str->len;
    ++*col;

    while (str->len > 0 && str->ptr[0] >= '0' && str->ptr[0] <= '7') {
      result *= 8;

      if (str->ptr[0] >= '0' && str->ptr[0] <= '7')
        result += str->ptr[0] - '0';

      ++str->ptr;
      --str->len;
      ++*col;
    }

    --str->ptr;
    ++str->len;
    ++*col;

    return result;
  }

  default: return _char;
  }
}

static TokenStatus lex(Lexer *lexer, Token *token, Str file_path) {
  if (lexer->code.len > 0) {
    u64 id = 0;
    u32 char_len;
    Str lexeme = table_matches(lexer->table, &lexer->code, &id, &char_len);
    u16 row = lexer->row;
    u16 col = lexer->col;

    if (id == TT_NEWLINE) {
      ++lexer->row;
      lexer->col = 0;

      return TokenStatusEmpty;
    } else if (id == TT_COMMENT) {
      u32 next_len;
      wchar next;

      while ((next = get_next_wchar(lexer->code, 0, &next_len)) != U'\0' &&
             next != U'\n') {
        lexer->code.ptr += next_len;
        lexer->code.len -= next_len;
      }

      return TokenStatusEmpty;
    } else if (id == TT_WHITESPACE) {
      lexer->col += char_len;

      return TokenStatusEmpty;
    }

    if (id == (u64) -1) {
      u32 wchar_len;
      wchar _wchar = get_next_wchar(lexer->code, 0, &wchar_len);

      PERROR(STR_FMT":%u:%u: ", "Unexpected `%lc`\n", STR_ARG(file_path),
             lexer->row + 1, lexer->col + 1, (wint_t) _wchar);
      exit(1);
    }

    if (id == TT_STR || id == TT_FSTR) {
      if (id == TT_FSTR)
        sb_push_char(&lexer->temp_sb, lexer->code.ptr[-2]);
      sb_push_char(&lexer->temp_sb, lexer->code.ptr[-1]);

      char _char;
      if (id == TT_STR)
        _char = lexer->temp_sb.buffer[0];
      else
        _char = lexer->temp_sb.buffer[1];

      bool is_escaped = false;
      while (lexer->code.len > 0 && (lexer->code.ptr[0] != _char || is_escaped)) {
        u32 next_len;
        wchar next = get_next_wchar(lexer->code, 0, &next_len);

        if (is_escaped || next != U'\\') {
          if (is_escaped) {
            sb_push_char(&lexer->temp_sb, escape_char(&lexer->code, &lexer->col));
          } else {
            for (u32 i = 0; i < next_len; ++i)
              sb_push_char(&lexer->temp_sb, lexer->code.ptr[i]);
          }
        }

        if (is_escaped)
          is_escaped = false;
        else if (next == U'\\')
          is_escaped = true;

        lexer->code.ptr += next_len;
        lexer->code.len -= next_len;
        ++lexer->col;
      }

      if (lexer->code.len == 0) {
        PERROR(STR_FMT":%u:%u: ", "String literal was not closed\n",
               STR_ARG(file_path), row + 1, col + 1);
        exit(1);
      }

      sb_push_char(&lexer->temp_sb, lexer->code.ptr[0]);

      ++lexer->code.ptr;
      --lexer->code.len;
      ++lexer->col;

      lexeme.len = lexer->temp_sb.len;
      lexeme.ptr = arena_alloc(lexer->arena, lexeme.len);
      memcpy(lexeme.ptr, lexer->temp_sb.buffer, lexeme.len);

      lexer->temp_sb.len = 0;
    } else {
      lexer->col += char_len;
    }

    *token = (Token) { id, lexeme, row, col };

    return TokenStatusOk;
  }

  return TokenStatusEOF;
}

static Token *token_process_if_ident(Token *token) {
  if (token->id == TT_IDENT)
    for (u32 i = 0; i < token->lexeme.len; ++i)
      if (token->lexeme.ptr[i] == '-')
        token->lexeme.ptr[i] = '_';
  return token;
}

static Token *parser_next_token(Parser *parser) {
  if (parser->index == parser->tokens.len)
    return NULL;
  return token_process_if_ident(parser->tokens.items + parser->index++);
}

static Token *parser_peek_token(Parser *parser) {
  if (parser->index == parser->tokens.len)
    return NULL;
  return token_process_if_ident(parser->tokens.items + parser->index);
}

static void print_id_mask(u64 id_mask) {
  u32 ids_count = 0;
  for (u64 i = 0; i < ARRAY_LEN(token_names); ++i)
    if (MASK(i) & id_mask)
      ++ids_count;

  for (u64 i = 0, j = 0; i < 64 && j < ids_count; ++i) {
    if ((1lu << i) & id_mask) {
      if (j > 0) {
        if (j + 1 == ids_count)
          fputs(" or ", stderr);
        else
          fputs(", ", stderr);
      }

      fputs(token_names[i], stderr);

      ++j;
    }
  }
}

static Token *parser_expect_token(Parser *parser, u64 id_mask) {
  Token *token = parser_next_token(parser);
  if (!token) {
    PERROR(STR_FMT": ", "Expected ", STR_ARG(parser->file_path));
    print_id_mask(id_mask);
    fprintf(stderr, ", but got EOF\n");
    exit(1);
  }

  if (MASK(token->id) & id_mask)
    return token;

  PERROR(STR_FMT":%u:%u: ", "Expected ",
         STR_ARG(parser->file_path),
         token->row + 1, token->col + 1);
  print_id_mask(id_mask);
  fprintf(stderr, ", but got `"STR_FMT"`\n",
          STR_ARG(token->lexeme));
  exit(1);
}

static void include_file(Strs *included_files, Str new_file) {
  for (u32 i = 0; i < included_files->len; ++i)
    if (str_eq(included_files->items[i], new_file))
      return;

  DA_APPEND(*included_files, new_file);
}

static Expr     *parser_parse_expr(Parser *parser, bool is_in_bin_op);
static Exprs     parser_parse_block(Parser *parser, u64 end_id_mask);
static ExprFunc  parser_parse_func(Parser *parser);
static void      parser_parse_macro_def(Parser *parser);

Exprs parse_ex(Str code, Str file_path, Macros *macros,
               Strs *included_files, Strs *include_paths,
               CachedASTs *cached_asts, Arena *arena) {
  u64 code_hash = str_hash(code);

  for (u32 i = 0; i < cached_asts->len; ++i) {
    CachedAST *cached_ast = cached_asts->items + i;

    if (cached_ast->code_hash == code_hash) {
      DA_EXTEND(*macros, cached_ast->macros);
      DA_EXTEND(*included_files, cached_ast->included_files);

      return cached_ast->ast;
    }
  }

  include_file(included_files, file_path);

  Parser parser = {0};

  Lexer lexer = {0};
  lexer.code = code;
  lexer.table = get_transition_table();
  lexer.arena = arena;

  TokenStatus status = TokenStatusEmpty;
  Token token;
  while (status != TokenStatusEOF) {
    status = lex(&lexer, &token, file_path);
    if (status != TokenStatusEOF && status != TokenStatusEmpty)
      DA_APPEND(parser.tokens, token);
  }

  free(lexer.temp_sb.buffer);

  parser.macros = macros;
  parser.file_path = file_path;
  parser.included_files = included_files;
  parser.cached_asts = cached_asts;
  parser.include_paths = include_paths;
  parser.arena = arena;

  Exprs ast = {0};

  Token *token_ptr = parser_peek_token(&parser);
  while (token_ptr) {
    parser_expect_token(&parser, MASK(TT_OPAREN));

    token_ptr = parser_expect_token(&parser, MASK(TT_FUN) | MASK(TT_MACRO));

    if (token_ptr->id == TT_FUN) {
      Expr *expr = arena_alloc(arena, sizeof(Expr));
      expr->kind = ExprKindFunc;
      expr->as.func = parser_parse_func(&parser);
      DA_ARENA_APPEND(ast, expr, arena);
    } else {
      parser_parse_macro_def(&parser);
    }

    token_ptr = parser_peek_token(&parser);
  }

  if (parser.label_indices.items)
    free(parser.label_indices.items);

  if (parser.tokens.items)
    free(parser.tokens.items);

  Macros cached_macros;
  cached_macros.len = macros->len;
  cached_macros.cap = cached_macros.len;
  cached_macros.items = arena_alloc(arena, cached_macros.cap * sizeof(Macro));
  memcpy(cached_macros.items, macros->items, cached_macros.len * sizeof(Macro));

  Strs cached_included_files;
  cached_included_files.len = included_files->len;
  cached_included_files.cap = cached_included_files.len;
  cached_included_files.items = arena_alloc(arena, cached_included_files.cap * sizeof(Str));
  memcpy(cached_included_files.items, included_files->items,
         cached_included_files.len * sizeof(Str));

  CachedAST cached_ast = {
    code_hash, ast, cached_macros,
    cached_included_files,
  };
  DA_APPEND(*cached_asts, cached_ast);

  return ast;
}

static Expr *parser_parse_local_expr(Str code, Str file_path,
                                     u32 *row, u32 *col,
                                     Macros *macros, Arena *arena) {
  Parser parser = {0};

  Lexer lexer = {0};
  lexer.code = code;
  lexer.row = *row;
  lexer.col = *col;
  lexer.table = get_transition_table();
  lexer.arena = arena;

  TokenStatus status = TokenStatusEmpty;
  Token token;
  while (status != TokenStatusEOF) {
    status = lex(&lexer, &token, file_path);
    if (status != TokenStatusEOF && status != TokenStatusEmpty)
      DA_APPEND(parser.tokens, token);
  }

  free(lexer.temp_sb.buffer);

  Strs included_files = {0};
  CachedASTs cached_asts = {0};
  Strs include_paths = {0};

  parser.macros = macros;
  parser.file_path = file_path;
  parser.included_files = &included_files;
  parser.cached_asts = &cached_asts;
  parser.include_paths = &include_paths;
  parser.arena = arena;

  Expr *local_expr = parser_parse_expr(&parser, false);

  if (parser.label_indices.items)
    free(parser.label_indices.items);

  if (parser.tokens.items)
    free(parser.tokens.items);

  if (included_files.items)
    free(included_files.items);

  if (cached_asts.items)
    free(cached_asts.items);

  if (include_paths.items)
    free(include_paths.items);

  *row = lexer.row;
  *col = lexer.col;

  return local_expr;
}

static void parser_parse_macro_def(Parser *parser) {
  Macro macro = {0};

  Token *name_token = parser_expect_token(parser, MASK(TT_IDENT));
  macro.name = name_token->lexeme;
  macro.row = name_token->row;
  macro.col = name_token->col;

  parser_expect_token(parser, MASK(TT_OPAREN));

  Token *next_token = parser_peek_token(parser);
  while (next_token && next_token->id != TT_CPAREN) {
    Token *arg_token = parser_expect_token(parser, MASK(TT_IDENT) | MASK(TT_UNPACK));
    if (arg_token->id == TT_UNPACK) {
      macro.has_unpack = true;

      arg_token = parser_expect_token(parser, MASK(TT_IDENT));
      DA_ARENA_APPEND(macro.arg_names, arg_token->lexeme, parser->arena);

      break;
    }

    DA_ARENA_APPEND(macro.arg_names, arg_token->lexeme, parser->arena);

    next_token = parser_peek_token(parser);
  }

  parser_expect_token(parser, MASK(TT_CPAREN));

  macro.body = parser_parse_block(parser, MASK(TT_CPAREN));
  parser_expect_token(parser, MASK(TT_CPAREN));

  DA_APPEND(*parser->macros, macro);
}

static ExprFunc parser_parse_func(Parser *parser) {
  ExprFunc result = {0};

  Token *token = parser_peek_token(parser);
  if (token && token->id == TT_IDENT)
    result.name = parser_expect_token(parser, MASK(TT_IDENT))->lexeme;

  parser_expect_token(parser, MASK(TT_OPAREN));
  Token *arg_token = parser_expect_token(parser, MASK(TT_IDENT) | MASK(TT_CPAREN));
  while (arg_token && arg_token->id != TT_CPAREN) {
    DA_ARENA_APPEND(result.args, arg_token->lexeme, parser->arena);

    arg_token = parser_expect_token(parser, MASK(TT_IDENT) |
                                            MASK(TT_CPAREN));
  }

  u32 prev_func = parser->current_func;

  result.body = parser_parse_block(parser, MASK(TT_CPAREN));
  parser_expect_token(parser, MASK(TT_CPAREN));

  parser->current_func = prev_func;

  return result;
}

static ExprIf parser_parse_if(Parser *parser) {
  parser_next_token(parser);

  ExprIf result = {0};
  result.cond = parser_parse_expr(parser, false);
  result.if_body = parser_parse_block(parser, MASK(TT_CPAREN) |
                                              MASK(TT_ELIF) |
                                              MASK(TT_ELSE));

  ExprIf *last = &result;

  Token *first_token = parser_expect_token(parser, MASK(TT_CPAREN) |
                                                   MASK(TT_ELIF) |
                                                   MASK(TT_ELSE));
  Token *next_token = first_token;

  while (next_token && next_token->id == TT_ELIF) {
    Expr *elif = arena_alloc(parser->arena, sizeof(Expr));
    elif->kind = ExprKindIf;
    elif->as._if.cond = parser_parse_expr(parser, false);
    elif->as._if.if_body = parser_parse_block(parser, MASK(TT_CPAREN) |
                                               MASK(TT_ELIF) |
                                               MASK(TT_ELSE));
    elif->loc.file_path = parser->file_path;
    elif->loc.row = next_token->row;
    elif->loc.col = next_token->col;

    last->else_body.len = 1;
    last->else_body.items = arena_alloc(parser->arena, sizeof(Expr *));
    last->else_body.items[0] = elif;
    last = &elif->as._if;

    next_token = parser_expect_token(parser, MASK(TT_CPAREN) |
                                             MASK(TT_ELIF) |
                                             MASK(TT_ELSE));
  }

  if (next_token->id == TT_ELSE) {
    last->else_body = parser_parse_block(parser, MASK(TT_CPAREN));

    parser_expect_token(parser, MASK(TT_CPAREN));
  }

  return result;
}

static Str get_file_dir(Str path) {
  for (u32 i = path.len; i > 0; --i)
    if (path.ptr[i - 1] == '/')
      return (Str) { path.ptr, i };

  return (Str) {0};
}

static ErBinOpKind token_id_to_bin_op_kind(u64 id) {
  switch (id) {
  case TT_PLUS:  return ErBinOpKindAdd;
  case TT_MINUS: return ErBinOpKindSub;
  case TT_STAR:  return ErBinOpKindMul;
  case TT_SLASH: return ErBinOpKindDiv;
  case TT_PERC:  return ErBinOpKindRem;
  case TT_EQ:    return ErBinOpKindEq;
  case TT_NE:    return ErBinOpKindNe;
  case TT_LS:    return ErBinOpKindLs;
  case TT_LE:    return ErBinOpKindLe;
  case TT_GT:    return ErBinOpKindGt;
  case TT_GE:    return ErBinOpKindGe;
  }

  return 0;
}

static void collect_lambda_arity(Exprs *args, u32 *arity) {
  for (u32 i = 0; i < args->len; ++i) {
    Expr *arg = args->items[i];
    if (arg->kind == ExprKindIdent) {
      if (arg->as.ident.name.ptr[0] == '$') {
        u32 index = str_to_u32(STR(arg->as.ident.name.ptr + 1,
                                   arg->as.ident.name.len - 1));
        if (*arity <= index)
          *arity = index + 1;
      }
    } else if (arg->kind == ExprKindBinOp) {
      collect_lambda_arity(&arg->as.bin_op.args, arity);
    }
  }
}

static Expr *parser_parse_expr(Parser *parser, bool is_in_bin_op) {
  Expr *expr = arena_alloc(parser->arena, sizeof(Expr));

  Token *first_token = parser_expect_token(parser,
                                           MASK(TT_OPAREN) | MASK(TT_STR) |
                                           MASK(TT_FSTR) | MASK(TT_IDENT) |
                                           MASK(TT_INT) | MASK(TT_BOOL) |
                                           MASK(TT_OCURLY) | MASK(TT_PLUS) |
                                           MASK(TT_MINUS) | MASK(TT_STAR) |
                                           MASK(TT_SLASH) | MASK(TT_PERC) |
                                           MASK(TT_EQ) | MASK(TT_NE) |
                                           MASK(TT_LS) | MASK(TT_LE) |
                                           MASK(TT_GT) | MASK(TT_GE));

  expr->loc.file_path = parser->file_path;
  expr->loc.row = first_token->row;
  expr->loc.col = first_token->col;

  bool found_atom = true;

  switch (first_token->id) {
  case TT_STR: {
    expr->kind = ExprKindStr;
    expr->as.str.str = STR(first_token->lexeme.ptr + 1,
                           first_token->lexeme.len - 2);
  } break;

  case TT_FSTR: {
    Str str = {
      first_token->lexeme.ptr + 2,
      first_token->lexeme.len - 3,
    };

    FStrParts parts = {0};

    u32 anchor = 0;
    u32 index = 0;
    u32 row_offset = 0;
    u32 col_offset = 0;
    while (index < str.len) {
      bool current_is_obrace = str.ptr[index] == '{';
      bool next_is_obrace = index + 1 < str.len && str.ptr[index + 1] == '{';
      bool next_is_cbrace = index + 1 < str.len && str.ptr[index + 1] == '}';
      if (current_is_obrace && !next_is_obrace && !next_is_cbrace) {
        if (anchor < index) {
          FStrPart part = {
            {
              str.ptr + anchor,
              index - anchor,
            },
            NULL,
          };
          DA_APPEND(parts, part);
        }

        index += 1;
        col_offset += 1;

        u32 base_row = expr->loc.row + row_offset;
        u32 base_col = expr->loc.col + col_offset + 2;
        u32 row = base_row;
        u32 col = base_col;
        Str code = { str.ptr + index, 0 };

        while (code.len < str.len - index && code.ptr[code.len] != '}')
          ++code.len;

        Expr *local_expr = parser_parse_local_expr(code, parser->file_path,
                                                   &row, &col, parser->macros,
                                                   parser->arena);

        index += code.len;
        row_offset += row - base_row;
        col_offset += col - base_col;

        if (index == str.len) {
          ERROR(STR_FMT":%u:%u: Expected `}`, but got EOS\n",
                STR_ARG(parser->file_path),
                expr->loc.row + row_offset + 1,
                expr->loc.col + col_offset + 1);
          exit(1);
        } else if (str.ptr[index] != '}') {
          ERROR(STR_FMT":%u:%u: Expected `}`, but got `%c`\n",
                STR_ARG(parser->file_path),
                expr->loc.row + row_offset + 1,
                expr->loc.col + col_offset + 1,
                str.ptr[index]);
          exit(1);
        }

        FStrPart part = {
          {},
          local_expr,
        };
        DA_APPEND(parts, part);

        index += 1;
        col_offset += 1;
        anchor = index;
      } else if (current_is_obrace && next_is_obrace) {
        if (str.ptr[index] == '\n') {
          row_offset += 1;
          col_offset = 0;
        } else {
          col_offset += 2;
        }
        index += 2;
      } else {
        if (str.ptr[index] == '\n') {
          row_offset += 1;
          col_offset = 0;
        } else {
          col_offset += 1;
        }
        index += 1;
      }
    }

    if (anchor < index) {
      FStrPart part = {
        {
          str.ptr + anchor,
          index - anchor,
        },
        NULL,
      };
      DA_APPEND(parts, part);
    }

    FStrParts new_parts;
    new_parts.len = parts.len;
    new_parts.cap = new_parts.len;
    if (new_parts.len > 0) {
      new_parts.items = arena_alloc(parser->arena, new_parts.cap * sizeof(FStrPart));
      memcpy(new_parts.items, parts.items, new_parts.len * sizeof(FStrPart));
      free(parts.items);
    }

    expr->kind = ExprKindFStr;
    expr->as.fstr.parts = new_parts;
  } break;

  case TT_IDENT: {
    expr->kind = ExprKindIdent;
    expr->as.ident.name = first_token->lexeme;
  } break;

  case TT_INT: {
    expr->kind = ExprKindInt;
    expr->as._int._int = str_to_i64(first_token->lexeme);
  } break;

  case TT_BOOL: {
    expr->kind = ExprKindBool;
    expr->as._bool._bool = str_eq(first_token->lexeme, STR_LIT("true"));
  } break;

  case TT_OCURLY: {
    expr->kind = ExprKindList;

    Token *token = parser_peek_token(parser);
    while (token && token->id != TT_CCURLY) {
      Expr *temp_expr = parser_parse_expr(parser, false);
      DA_ARENA_APPEND(expr->as.list.elements, temp_expr, parser->arena);

      token = parser_peek_token(parser);
    }

    parser_expect_token(parser, MASK(TT_CCURLY));
  } break;

  case TT_PLUS:
  case TT_MINUS:
  case TT_STAR:
  case TT_SLASH:
  case TT_PERC:
  case TT_EQ:
  case TT_NE:
  case TT_LS:
  case TT_LE:
  case TT_GT:
  case TT_GE: {
    expr->kind = ExprKindFunc;
    expr->as.func.args.len = 2;
    expr->as.func.args.cap = expr->as.func.args.len;
    expr->as.func.args.items =
      arena_alloc(parser->arena, expr->as.func.args.cap * sizeof(Str));
    expr->as.func.args.items[0] = STR_LIT("$0");
    expr->as.func.args.items[1] = STR_LIT("$1");

    Expr *bin_op = arena_alloc(parser->arena, sizeof(Expr));
    bin_op->kind = ExprKindBinOp;
    bin_op->as.bin_op.kind = token_id_to_bin_op_kind(first_token->id);
    bin_op->as.bin_op.args.len = 2;
    bin_op->as.bin_op.args.cap = bin_op->as.bin_op.args.len;
    bin_op->as.bin_op.args.items =
      arena_alloc(parser->arena, bin_op->as.bin_op.args.cap * sizeof(Expr *));
    bin_op->as.bin_op.args.items[0] = arena_alloc(parser->arena, sizeof(Expr));
    bin_op->as.bin_op.args.items[0]->kind = ExprKindIdent;
    bin_op->as.bin_op.args.items[0]->as.ident.name = STR_LIT("$0");
    bin_op->as.bin_op.args.items[1] = arena_alloc(parser->arena, sizeof(Expr));
    bin_op->as.bin_op.args.items[1]->kind = ExprKindIdent;
    bin_op->as.bin_op.args.items[1]->as.ident.name = STR_LIT("$1");

    expr->as.func.body.len = 1;
    expr->as.func.body.cap = expr->as.func.body.len;
    expr->as.func.body.items =
      arena_alloc(parser->arena, expr->as.func.body.cap * sizeof(Expr *));
    expr->as.func.body.items[0] = bin_op;
  } break;

  default: {
    found_atom = false;
  } break;
  }

  if (!found_atom) {
    Token *token = parser_peek_token(parser);

    switch (token->id) {
    case TT_FUN: {
      parser_next_token(parser);

      expr->kind = ExprKindFunc;
      expr->as.func = parser_parse_func(parser);
    } break;

    case TT_LET: {
      parser_next_token(parser);

      Token *name_token = parser_expect_token(parser, MASK(TT_IDENT));

      expr->kind = ExprKindLet;
      expr->as.let.name = name_token->lexeme;
      expr->as.let.value = parser_parse_expr(parser, false);

      parser_expect_token(parser, MASK(TT_CPAREN));
    } break;

    case TT_IF: {
      expr->kind = ExprKindIf;
      expr->as._if = parser_parse_if(parser);
    } break;

    case TT_PLUS:
    case TT_MINUS:
    case TT_STAR:
    case TT_SLASH:
    case TT_PERC:
    case TT_EQ:
    case TT_NE:
    case TT_LS:
    case TT_LE:
    case TT_GT:
    case TT_GE: {
      parser_next_token(parser);

      expr->kind = ExprKindBinOp;
      expr->as.bin_op.kind = token_id_to_bin_op_kind(token->id);

      u32 lambda_arity = 0;

      Token *token = parser_peek_token(parser);
      while (token && token->id != TT_CPAREN) {
        token = parser_peek_token(parser);
        if (token->id == TT_DOLLAR) {
          parser_next_token(parser);
          token = parser_expect_token(parser, MASK(TT_INT));

          u32 index = str_to_u32(token->lexeme);
          if (lambda_arity <= index)
            lambda_arity = index + 1;

          Expr *temp_expr = arena_alloc(parser->arena, sizeof(Expr));
          temp_expr->kind = ExprKindIdent;

          StringBuilder sb = {0};
          sb_push_char(&sb, '$');
          sb_push_str(&sb, token->lexeme);
          temp_expr->as.ident.name.len = sb.len;
          temp_expr->as.ident.name.ptr =
            arena_alloc(parser->arena, temp_expr->as.ident.name.len);
          memcpy(temp_expr->as.ident.name.ptr, sb.buffer, sb.len);
          free(sb.buffer);

          DA_ARENA_APPEND(expr->as.bin_op.args, temp_expr, parser->arena);
        } else {
          Expr *temp_expr = parser_parse_expr(parser, true);

          if (!is_in_bin_op && temp_expr->kind == ExprKindBinOp)
            collect_lambda_arity(&temp_expr->as.bin_op.args, &lambda_arity);

          DA_ARENA_APPEND(expr->as.bin_op.args, temp_expr, parser->arena);
        }

        token = parser_peek_token(parser);
      }

      parser_expect_token(parser, MASK(TT_CPAREN));

      if (lambda_arity > 0 && !is_in_bin_op) {
        StringBuilder sb = {0};

        Expr *temp_expr = arena_alloc(parser->arena, sizeof(Expr));
        temp_expr->kind = ExprKindFunc;

        for (u32 i = 0; i < lambda_arity; ++i) {
          sb_push_char(&sb, '$');
          sb_push_u32(&sb, i);

          Str arg;
          arg.len = sb.len;
          arg.ptr = arena_alloc(parser->arena, arg.len);
          memcpy(arg.ptr, sb.buffer, sb.len);

          DA_ARENA_APPEND(temp_expr->as.func.args, arg, parser->arena);

          sb.len = 0;
        }

        temp_expr->as.func.body.len = 1;
        temp_expr->as.func.body.cap = temp_expr->as.func.body.len;
        temp_expr->as.func.body.items =
          arena_alloc(parser->arena, temp_expr->as.func.body.cap * sizeof(Expr *));
        temp_expr->as.func.body.items[0] = expr;
        expr = temp_expr;

        free(sb.buffer);
      }
    } break;

    case TT_MACRO: {
      parser_next_token(parser);
      parser_parse_macro_def(parser);

      expr->kind = ExprKindBlock;
    } break;

    case TT_USE: {
      parser_next_token(parser);

      Str module_path = parser_expect_token(parser, MASK(TT_STR))->lexeme;
      module_path.ptr += 1;
      module_path.len -= 2;

      parser_expect_token(parser, MASK(TT_CPAREN));

      expr->kind = ExprKindBlock;

      StringBuilder path_sb = {0};
      Str code = { NULL, (u32) -1 };
      Str path = {0};

      for (u32 i = 0; i < parser->include_paths->len; ++i) {
        sb_push_str(&path_sb, parser->include_paths->items[i]);
        sb_push_str(&path_sb, module_path);
        sb_push_str(&path_sb, STR_LIT(".er\0"));

        code = read_file_arena(path_sb.buffer, parser->arena);

        if (code.len != (u32) -1) {
          --path_sb.len; // exclude NULL-terminator
          path = sb_to_str(path_sb);

          break;
        }

        path_sb.len = 0;
      }

      if (path_sb.buffer)
        free(path_sb.buffer);

      if (code.len == (u32) -1) {
        PERROR(STR_FMT":%u:%u: ", "Could not import `"STR_FMT"` module\n",
               STR_ARG(parser->file_path), token->row + 1,
               token->col + 1, STR_ARG(module_path));
        exit(1);
      }

      for (u32 i = 0; i < parser->included_files->len; ++i)
        if (str_eq(parser->included_files->items[i], path))
          return expr;

      Str dir = get_file_dir(path);
      DA_APPEND(*parser->include_paths, dir);

      include_file(parser->included_files, path);

      Arena arena = {0};
      expr->as.block = parse_ex(code, path, parser->macros,
                                parser->included_files, parser->include_paths,
                                parser->cached_asts, &arena);
    } break;

    case TT_SET: {
      parser_next_token(parser);

      expr->kind = ExprKindSet;
      expr->as.set.name = parser_next_token(parser)->lexeme;
      expr->as.set.value = parser_parse_expr(parser, false);

      parser_expect_token(parser, MASK(TT_CPAREN));
    } break;

    case TT_RET: {
      parser_next_token(parser);

      expr->kind = ExprKindRet;

      token = parser_peek_token(parser);
      if (token && token->id != TT_CPAREN)
        expr->as.ret.value = parser_parse_expr(parser, false);

      parser_expect_token(parser, MASK(TT_CPAREN));
    } break;

    case TT_DO: {
      parser_next_token(parser);

      expr->kind = ExprKindBlock;
      expr->as.block = parser_parse_block(parser, MASK(TT_CPAREN));

      parser_expect_token(parser, MASK(TT_CPAREN));
    } break;

    default: {
      expr->kind = ExprKindFuncCall;
      expr->as.func_call.func = parser_parse_expr(parser, false);
      expr->as.func_call.built_in = NULL;
      expr->as.func_call.args = parser_parse_block(parser, MASK(TT_CPAREN));
      parser_expect_token(parser, MASK(TT_CPAREN));
    } break;
    }
  }

  return expr;
}

static Exprs parser_parse_block(Parser *parser, u64 end_id_mask) {
  Exprs result = {0};

  Token *token = parser_peek_token(parser);
  while (token && !(MASK(token->id) & end_id_mask)) {
    Expr *expr = parser_parse_expr(parser, false);
    DA_ARENA_APPEND(result, expr, parser->arena);

    token = parser_peek_token(parser);
  }

  return result;
}

Exprs parse(Str code, Str file_path, Strs *include_paths,
            CachedASTs *cached_asts, Macros *macros, Arena *arena) {
  Strs included_files = {0};

  DA_APPEND(included_files, file_path);

  Exprs result = parse_ex(code, file_path, macros,
                          &included_files, include_paths,
                          cached_asts, arena);

  free(included_files.items);

  return result;
}
