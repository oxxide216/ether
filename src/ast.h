#ifndef AST_H
#define AST_H

#include "arena.h"
#include "shl/shl-defs.h"
#include "shl/shl-str.h"

typedef enum {
  TypeKindUnit = 0,
  TypeKindFunc,
  TypeKindInt,
  TypeKindBool,
  TypeKindStr,
} TypeKind;

typedef struct Type Type;
typedef Da(Type) Types;

struct Type {
  TypeKind kind;
  union {
    struct {
      Type  *return_type;
      Types  arg_types;
    };
  };
};

typedef struct Expr Expr;
typedef Da(Expr *) Exprs;

typedef enum {
  ExprKindStr = 0,
  ExprKindInt,
  ExprKindBool,
  ExprKindBlock,
  ExprKindIdent,
  ExprKindFunc,
  ExprKindFuncCall,
  ExprKindLet,
  ExprKindSet,
  ExprKindRet,
  ExprKindIf,
  ExprKindBinOp,
} ExprKind;

typedef struct  {
  Str str;
} ExprStr;

typedef struct  {
  i64 _int;
} ExprInt;

typedef struct {
  bool _bool;
} ExprBool;

typedef struct  {
  Str name;
} ExprIdent;

typedef struct {
  Str  name;
  Type type;
} ErArg;

typedef Da(ErArg) ErArgs;

typedef struct {
  Str    name;
  ErArgs args;
  Type   return_type;
  Exprs  body;
} ExprFunc;

typedef struct {
  Str   name;
  Exprs args;
} ExprFuncCall;

typedef struct {
  Str   name;
  Expr *value;
} ExprLet;

typedef struct {
  Str   name;
  Expr *value;
} ExprSet;

typedef struct {
  Expr *value;
} ExprRet;

typedef struct {
  Expr  *cond;
  Exprs  if_body;
  Exprs  else_body;
} ExprIf;

typedef enum {
  ErBinOpKindAdd = 0,
  ErBinOpKindSub,
  ErBinOpKindMul,
  ErBinOpKindDiv,
  ErBinOpKindRem,
} ErBinOpKind;

typedef struct {
  ErBinOpKind kind;
  Exprs       args;
} ExprBinOp;

typedef union {
  ExprStr      str;
  ExprInt      _int;
  ExprBool     _bool;
  Exprs        block;
  ExprIdent    ident;
  ExprFunc     func;
  ExprFuncCall func_call;
  ExprLet      let;
  ExprSet      set;
  ExprRet      ret;
  ExprIf       _if;
  ExprBinOp    bin_op;
} ExprAs;

typedef struct {
  Str file_path;
  u32 row, col;
} ExprLoc;

struct Expr {
  ExprKind kind;
  ExprAs   as;
  ExprLoc  loc;
};

typedef struct {
  Str  name;
  Type type;
} Var;

typedef Da(Var) Vars;

typedef struct {
  ExprFunc *expr;
  Vars      vars;
} Func;

typedef Da(Func) Funcs;

Exprs ast_clone(Exprs *ast, Arena *arena);

Type type_clone(Type *type, Arena *arena);
bool type_eq(Type *a, Type *b);
void type_free(Type *type);

u32 get_type_size(Type *type);

u32 get_var_index(Vars *vars, u32 top, Str name);

#endif // AST_H
