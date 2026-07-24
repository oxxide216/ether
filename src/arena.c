#include <stdlib.h>

#include "arena.h"

void *arena_alloc(Arena *arena, u32 size) {
  if (size > ARENA_MAX_IGNORED_SIZE) {
    while (arena->beginning &&
           arena->beginning->cap - arena->beginning->len <= ARENA_MAX_IGNORED_SIZE)
    arena->beginning = arena->beginning->next;
  }

  Segment *segment = arena->beginning;
  Segment **segment_next = &arena->beginning;
  while (segment) {
    if (segment->len + size <= segment->cap) {
      void *result = segment->space + segment->len;
      segment->len += size;

      return result;
    }

    segment_next = &segment->next;
    segment = segment->next;
  }

  u32 new_cap = DEFAULT_ARENA_SEGMENT_SIZE;
  if (new_cap < size)
    new_cap = size;

  (*segment_next) = malloc(sizeof(Segment) + new_cap);
  (*segment_next)->space = (*segment_next) + 1;
  (*segment_next)->len = size;
  (*segment_next)->cap = new_cap;
  (*segment_next)->next = NULL;

  memset((*segment_next)->space, 0, (*segment_next)->cap);

  if (!arena->segments)
    arena->segments = (*segment_next);

  return (*segment_next)->space;
}

void arena_reset(Arena *arena) {
  Segment *segment = arena->segments;
  while (segment) {
    segment->len = 0;

    memset(segment->space, 0, segment->cap);

    segment = segment->next;
  }

  arena->beginning = arena->segments;
}

void arena_free(Arena *arena) {
  Segment *segment = arena->segments;
  while (segment) {
    Segment *next = segment->next;
    free(segment);
    segment = next;
  }

  arena->segments = NULL;
  arena->beginning = NULL;
}
