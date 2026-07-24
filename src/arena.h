#ifndef ARENA_H
#define ARENA_H

#include "shl/shl-defs.h"

#define DEFAULT_ARENA_SEGMENT_SIZE 1024 * 4
#define ARENA_MAX_IGNORED_SIZE     16

typedef struct Segment Segment;

struct Segment {
  void    *space;
  u32      len, cap;
  Segment *next;
};

typedef struct {
  Segment *segments;
  Segment *beginning;
} Arena;

void *arena_alloc(Arena *arena, u32 size);
void  arena_reset(Arena *arena);
void  arena_free(Arena *arena);

#endif // ARENA_H
