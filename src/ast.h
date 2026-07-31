#ifndef AST_H
#define AST_H

#include "arena.h"
#include "shl/shl-defs.h"
#include "shl/shl-str.h"

typedef Da(Str) Strs;

typedef struct BuiltIn BuiltIn;

typedef enum {
  TypeKindUnit = 0,
  TypeKindFunc,
  TypeKindInt,
  TypeKindBool,
  TypeKindStr,
  TypeKindList,
  TypeKindAny,
} TypeKind;

typedef struct Type Type;
typedef Da(Type *) Types;

struct Type {
  TypeKind kind;
  union {
    struct {
      Type    *return_type;
      Types    arg_types;
      u32      func_index;
      BuiltIn *built_in;
    };
    Type *element_type;
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
  ExprKindFStr,
  ExprKindList,
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
  Str    name;
  Strs   args;
  Exprs  body;
} ExprFunc;

typedef struct {
  Expr    *func;
  Exprs    args;
  BuiltIn *built_in;
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
  ErBinOpKindEq,
  ErBinOpKindNe,
  ErBinOpKindLs,
  ErBinOpKindLe,
  ErBinOpKindGt,
  ErBinOpKindGe,
} ErBinOpKind;

typedef struct {
  ErBinOpKind kind;
  Exprs       args;
} ExprBinOp;

typedef struct {
  Str   str;
  Expr *expr;
} FStrPart;

typedef Da(FStrPart) FStrParts;

typedef struct {
  FStrParts parts;
} ExprFStr;

typedef struct {
  Exprs elements;
} ExprList;

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
  ExprFStr     fstr;
  ExprList     list;
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
  Str   name;
  Type *type;
} Var;

typedef Da(Var) Vars;

typedef struct {
  ExprFunc *expr;
  Type     *type;
  Strs      arg_names;
  Vars      vars;
  bool      is_lambda;
  bool      is_checked;
} Func;

typedef Da(Func) Funcs;

Type type_clone(Type *type, Arena *arena);
bool type_eq(Type *a, Type *b);
void type_free(Type *type);

u32 get_type_size(Type *type);

u32 get_var_index(Vars *vars, u32 top, Str name);
u32 get_func_index(Funcs *funcs, Str name);
u32 get_func_index_with_signature(Funcs *funcs, Str name, Types *arg_types);

#endif // AST_H
