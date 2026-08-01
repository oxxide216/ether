#ifndef LEXGEN_TRANSITION_TABLE
#define LEXGEN_TRANSITION_TABLE

#define TT_WHITESPACE 0
#define TT_NEWLINE 1
#define TT_COMMENT 2
#define TT_FUN 3
#define TT_LET 4
#define TT_IF 5
#define TT_ELIF 6
#define TT_ELSE 7
#define TT_MACRO 8
#define TT_SET 9
#define TT_USE 10
#define TT_RET 11
#define TT_DO 12
#define TT_PLUS 13
#define TT_MINUS 14
#define TT_STAR 15
#define TT_SLASH 16
#define TT_PERC 17
#define TT_OPAREN 18
#define TT_CPAREN 19
#define TT_OCURLY 20
#define TT_CCURLY 21
#define TT_STR 22
#define TT_FSTR 23
#define TT_UNPACK 24
#define TT_INT 25
#define TT_FLOAT 26
#define TT_BOOL 27
#define TT_IDENT 28

#define TTS_COLNT U29

TransitionTable *get_transition_table(void);

#ifdef LEXGEN_TRANSITION_TABLE_IMPLEMENTATION

TransitionCol table_col_whitespace[] = {
  { 1, 32, 32, 1 },
  { 1, 9, 9, 1 },
  { 1, 13, 13, 1 },
  { 1, -1, -1, 0 },
};

TransitionCol table_col_newline[] = {
  { 1, 10, 10, 0 },
};

TransitionCol table_col_comment[] = {
  { 1, 59, 59, 0 },
};

TransitionCol table_col_fun[] = {
  { 1, 102, 102, 2 },
  { 2, 117, 117, 3 },
  { 3, 110, 110, 0 },
};

TransitionCol table_col_let[] = {
  { 1, 108, 108, 2 },
  { 2, 101, 101, 3 },
  { 3, 116, 116, 0 },
};

TransitionCol table_col_if[] = {
  { 1, 105, 105, 2 },
  { 2, 102, 102, 0 },
};

TransitionCol table_col_elif[] = {
  { 1, 101, 101, 2 },
  { 2, 108, 108, 3 },
  { 3, 105, 105, 4 },
  { 4, 102, 102, 0 },
};

TransitionCol table_col_else[] = {
  { 1, 101, 101, 2 },
  { 2, 108, 108, 3 },
  { 3, 115, 115, 4 },
  { 4, 101, 101, 0 },
};

TransitionCol table_col_macro[] = {
  { 1, 109, 109, 2 },
  { 2, 97, 97, 3 },
  { 3, 99, 99, 4 },
  { 4, 114, 114, 5 },
  { 5, 111, 111, 0 },
};

TransitionCol table_col_set[] = {
  { 1, 115, 115, 2 },
  { 2, 101, 101, 3 },
  { 3, 116, 116, 0 },
};

TransitionCol table_col_use[] = {
  { 1, 117, 117, 2 },
  { 2, 115, 115, 3 },
  { 3, 101, 101, 0 },
};

TransitionCol table_col_ret[] = {
  { 1, 114, 114, 2 },
  { 2, 101, 101, 3 },
  { 3, 116, 116, 0 },
};

TransitionCol table_col_do[] = {
  { 1, 100, 100, 2 },
  { 2, 111, 111, 0 },
};

TransitionCol table_col_plus[] = {
  { 1, 43, 43, 0 },
};

TransitionCol table_col_minus[] = {
  { 1, 45, 45, 0 },
};

TransitionCol table_col_star[] = {
  { 1, 42, 42, 0 },
};

TransitionCol table_col_slash[] = {
  { 1, 47, 47, 0 },
};

TransitionCol table_col_perc[] = {
  { 1, 37, 37, 0 },
};

TransitionCol table_col_oparen[] = {
  { 1, 40, 40, 0 },
};

TransitionCol table_col_cparen[] = {
  { 1, 41, 41, 0 },
};

TransitionCol table_col_ocurly[] = {
  { 1, 91, 91, 0 },
};

TransitionCol table_col_ccurly[] = {
  { 1, 93, 93, 0 },
};

TransitionCol table_col_str[] = {
  { 1, 34, 34, 0 },
};

TransitionCol table_col_fstr[] = {
  { 1, 102, 102, 2 },
  { 2, 34, 34, 0 },
};

TransitionCol table_col_unpack[] = {
  { 1, 46, 46, 2 },
  { 2, 46, 46, 3 },
  { 3, 46, 46, 0 },
};

TransitionCol table_col_int[] = {
  { 1, 45, 45, 2 },
  { 1, -1, -1, 2 },
  { 2, 48, 57, 3 },
  { 3, 48, 57, 3 },
  { 3, -1, -1, 0 },
};

TransitionCol table_col_float[] = {
  { 1, 45, 45, 2 },
  { 1, -1, -1, 2 },
  { 2, 48, 57, 3 },
  { 3, 48, 57, 3 },
  { 3, -1, -1, 5 },
  { 5, 46, 46, 6 },
  { 6, 48, 57, 7 },
  { 7, 48, 57, 7 },
  { 7, -1, -1, 0 },
};

TransitionCol table_col_bool[] = {
  { 1, 116, 116, 2 },
  { 2, 114, 114, 3 },
  { 3, 117, 117, 4 },
  { 4, 101, 101, 0 },
  { 1, 102, 102, 2 },
  { 2, 97, 97, 3 },
  { 3, 108, 108, 4 },
  { 4, 115, 115, 5 },
  { 5, 101, 101, 0 },
};

TransitionCol table_col_ident[] = {
  { 1, 97, 122, 2 },
  { 1, 65, 90, 2 },
  { 1, 95, 95, 2 },
  { 1, 45, 45, 2 },
  { 2, 97, 122, 2 },
  { 2, 65, 90, 2 },
  { 2, 95, 95, 2 },
  { 2, 45, 45, 2 },
  { 2, 48, 57, 2 },
  { 2, -1, -1, 0 },
};

TransitionRow table_rows[] = {
  { table_col_whitespace, sizeof(table_col_whitespace) / sizeof(TransitionCol) },
  { table_col_newline, sizeof(table_col_newline) / sizeof(TransitionCol) },
  { table_col_comment, sizeof(table_col_comment) / sizeof(TransitionCol) },
  { table_col_fun, sizeof(table_col_fun) / sizeof(TransitionCol) },
  { table_col_let, sizeof(table_col_let) / sizeof(TransitionCol) },
  { table_col_if, sizeof(table_col_if) / sizeof(TransitionCol) },
  { table_col_elif, sizeof(table_col_elif) / sizeof(TransitionCol) },
  { table_col_else, sizeof(table_col_else) / sizeof(TransitionCol) },
  { table_col_macro, sizeof(table_col_macro) / sizeof(TransitionCol) },
  { table_col_set, sizeof(table_col_set) / sizeof(TransitionCol) },
  { table_col_use, sizeof(table_col_use) / sizeof(TransitionCol) },
  { table_col_ret, sizeof(table_col_ret) / sizeof(TransitionCol) },
  { table_col_do, sizeof(table_col_do) / sizeof(TransitionCol) },
  { table_col_plus, sizeof(table_col_plus) / sizeof(TransitionCol) },
  { table_col_minus, sizeof(table_col_minus) / sizeof(TransitionCol) },
  { table_col_star, sizeof(table_col_star) / sizeof(TransitionCol) },
  { table_col_slash, sizeof(table_col_slash) / sizeof(TransitionCol) },
  { table_col_perc, sizeof(table_col_perc) / sizeof(TransitionCol) },
  { table_col_oparen, sizeof(table_col_oparen) / sizeof(TransitionCol) },
  { table_col_cparen, sizeof(table_col_cparen) / sizeof(TransitionCol) },
  { table_col_ocurly, sizeof(table_col_ocurly) / sizeof(TransitionCol) },
  { table_col_ccurly, sizeof(table_col_ccurly) / sizeof(TransitionCol) },
  { table_col_str, sizeof(table_col_str) / sizeof(TransitionCol) },
  { table_col_fstr, sizeof(table_col_fstr) / sizeof(TransitionCol) },
  { table_col_unpack, sizeof(table_col_unpack) / sizeof(TransitionCol) },
  { table_col_int, sizeof(table_col_int) / sizeof(TransitionCol) },
  { table_col_float, sizeof(table_col_float) / sizeof(TransitionCol) },
  { table_col_bool, sizeof(table_col_bool) / sizeof(TransitionCol) },
  { table_col_ident, sizeof(table_col_ident) / sizeof(TransitionCol) },
};

TransitionTable table = {
  table_rows,
  sizeof(table_rows) / sizeof(TransitionRow),
};

TransitionTable *get_transition_table(void) {
  return &table;
};

#endif // LEXGEN_TRANSITION_TABLE_IMPLEMENTATION

#endif // LEXGEN_TRANSITION_TABLE
