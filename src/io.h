#include "arena.h"
#include "shl/shl-str.h"

Str read_file(char *path);
Str read_file_arena(char *path, Arena *arena);
bool write_file(char *path, Str content);
