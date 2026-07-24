#include "io.h"
#include "parser.h"
#include "evm-encoder.h"
#include "shl/shl-defs.h"
#define SHL_STR_IMPLEMENTATION
#include "shl/shl-str.h"
#include "shl/shl-log.h"

#define EVM_PREFIX  "libs/e/evm -o "
#define YASM_PREFIX "yasm -felf64 -o "
#define LD_PREFIX   "ld -o "

typedef struct {
  char *input_path;
  char *output_path;
  char *ir_path;
  char *asm_path;
  char *obj_path;
  bool  is_output_path_malloced;
  Strs  include_paths;
} Config;

static void print_usage(char *program_name) {
  fprintf(stderr, "Usage: %s <options...> <input file>\n\n", program_name);
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "       -o <output file>          Specify output file\n");
  fprintf(stderr, "       -I <search paths>         Specify included module search path\n");
}

static char *make_ir_path(char *input_path) {
  u32 len = strlen(input_path);
  u32 begin = len;
  while (begin > 0 && input_path[begin - 1] != '/')
    --begin;

  input_path += begin;
  len -= begin;

  if (len > 2 && strcmp(input_path + len - 3, ".er") == 0) {
    char *result = malloc(5 + len - 3 + 4 + 1);
    strcpy(result, "/tmp/");
    memcpy(result + 5, input_path, len - 3);
    strcpy(result + 5 + len - 3, ".eir");
    result[5 + len - 3 + 4] = '\0';
    return result;
  } else {
    char *result = malloc(5 + len + 5 + 1);
    strcpy(result, "/tmp/");
    strcpy(result + 5, input_path);
    strcpy(result + 5 + len, ".eir");
    result[5 + len + 5] = '\0';
    return result;
  }
}

static char *make_output_path(char *input_path) {
  u32 len = strlen(input_path);
  u32 begin = len;
  while (begin > 0 && input_path[begin - 1] != '/')
    --begin;

  input_path += begin;
  len -= begin;

  if (len > 2 && strcmp(input_path + len - 3, ".er") == 0) {
    char *result = malloc(len - 3 + 1);
    memcpy(result, input_path, len - 3);
    result[len - 3] = '\0';
    return result;
  } else {
    char *result = malloc(len + 1);
    memcpy(result, input_path, len);
    result[len] = '\0';
    return result;
  }
}

static char *make_asm_path(char *ir_path) {
  u32 len = strlen(ir_path);
  char *result = malloc(len - 4 + 2 + 1);
  memcpy(result, ir_path, len - 4);
  strcpy(result + len - 4, ".s");
  result[len - 4 + 2] = '\0';
  return result;
}

static char *make_obj_path(char *asm_path) {
  u32 len = strlen(asm_path);
  char *result = malloc(len + 1);
  memcpy(result, asm_path, len - 1);
  result[len - 1] = 'o';
  result[len] = '\0';
  return result;
}

static Config config_create(i32 argc, char **argv) {
  Config config = {0};

  for (u32 i = 1; i < (u32) argc; ++i) {
    if (strcmp(argv[i], "-o") == 0) {
      if (i + 1 == (u32) argc) {
        print_usage(argv[0]);
        ERROR("Option %s requires an argument\n", argv[i]);
        exit(1);
      }

      config.output_path = argv[++i];
    } else if (strcmp(argv[i], "-I") == 0) {
      if (i + 1 == (u32) argc) {
        print_usage(argv[0]);
        ERROR("Option %s requires an argument\n", argv[i]);
        exit(1);
      }

      Str include_path = str_new(argv[++i]);
      DA_APPEND(config.include_paths, include_path);
    } else if (argv[i][0] == '-') {
      print_usage(argv[0]);
      ERROR("Unknown option: %s\n", argv[i]);
      exit(1);
    } else {
      if (config.input_path) {
        print_usage(argv[0]);
        ERROR("More than one input file was provided\n");
        exit(1);
      }

      config.input_path = argv[i];
    }
  }

  if (!config.input_path) {
    print_usage(argv[0]);
    ERROR("Input file was not provided\n");
    exit(1);
  }

  if (!config.output_path) {
    config.output_path = make_output_path(config.input_path);
    config.is_output_path_malloced = true;
  }

  config.ir_path = make_ir_path(config.input_path);
  config.asm_path = make_asm_path(config.ir_path);
  config.obj_path = make_obj_path(config.asm_path);

#ifndef _WIN32
  DA_APPEND(config.include_paths, STR_LIT("/usr/include"));
#endif

  return config;
}

static void config_destroy(Config *config) {
  if (config->is_output_path_malloced)
    free(config->output_path);
  free(config->ir_path);
  free(config->asm_path);
  free(config->obj_path);
  if (config->include_paths.items)
    free(config->include_paths.items);
}

static void cached_asts_destroy(CachedASTs *asts) {
  if (asts->items)
    free(asts->items);
}

i32 main(i32 argc, char **argv) {
  Config config = config_create(argc, argv);

  Str code = read_file(config.input_path);
  if (code.len == (u32) -1) {
    ERROR("Could not read %s\n", config.input_path);
    config_destroy(&config);
    return 1;
  }

  Str input_path_str = str_new(config.input_path);
  CachedASTs cached_asts = {0};
  Arena arena = {0};
  Exprs ast = parse(code, input_path_str, &config.include_paths, &cached_asts, &arena);

  Funcs funcs = {0};
  Func func0 = { &ast.items[0]->as.func, {} };
  Var var0 = { STR_LIT("a"), {} };
  DA_APPEND(func0.vars, var0);
  Var var1 = { STR_LIT("b"), {} };
  DA_APPEND(func0.vars, var1);
  Var var2 = { STR_LIT("c"), {} };
  DA_APPEND(func0.vars, var2);
  DA_APPEND(funcs, func0);
  Func func1 = { &ast.items[1]->as.func, {} };
  DA_APPEND(funcs, func1);

  remove(config.ir_path);
  FILE *ir_file = fopen(config.ir_path, "wb");
  if (!ir_file) {
    ERROR("Could not write %s\n", config.ir_path);
    arena_free(&arena);
    cached_asts_destroy(&cached_asts);
    free(code.ptr);
    config_destroy(&config);
    return 1;
  }

  encode_ast_as_evm_ir(ir_file, &funcs);

  fclose(ir_file);

  i32 result = 0;
  StringBuilder sb = {0};

  sb_push(&sb, EVM_PREFIX);
  sb_push(&sb, config.asm_path);
  sb_push_char(&sb, ' ');
  sb_push(&sb, config.ir_path);
  sb_push_char(&sb, '\0');

  if (system(sb.buffer) != 0) {
    result = 1;
    goto end;
  }

  sb.len = 0;

  sb_push(&sb, YASM_PREFIX);
  sb_push(&sb, config.obj_path);
  sb_push_char(&sb, ' ');
  sb_push(&sb, config.asm_path);
  sb_push_char(&sb, '\0');

  if (system(sb.buffer) != 0) {
    result = 1;
    goto end;
  }

  sb.len = 0;

  sb_push(&sb, LD_PREFIX);
  sb_push(&sb, config.output_path);
  sb_push_char(&sb, ' ');
  sb_push(&sb, config.obj_path);
  sb_push_char(&sb, '\0');

  if (system(sb.buffer) != 0) {
    result = 1;
    goto end;
  }

end:
  free(sb.buffer);
  arena_free(&arena);
  cached_asts_destroy(&cached_asts);
  free(code.ptr);
  config_destroy(&config);

  return result;
}
