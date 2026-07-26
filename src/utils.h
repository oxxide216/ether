#ifndef UTILS_H
#define UTILS_H

#define DA_ARENA_APPEND(da, element, arena)                               \
  do {                                                                    \
    if ((da).cap <= (da).len) {                                           \
      if ((da).cap != 0) {                                                \
        while ((da).cap <= (da).len)                                      \
          (da).cap *= 2;                                                  \
        void *new_items = arena_alloc(arena, sizeof(element) * (da).cap); \
        memcpy(new_items,                                                 \
              (da).items,                                                 \
              (da).len * sizeof(element));                                \
        (da).items = new_items;                                           \
      } else {                                                            \
        (da).cap = 1;                                                     \
        (da).items = arena_alloc(arena, sizeof(element));                 \
      }                                                                   \
    }                                                                     \
    (da).items[(da).len++] = element;                                     \
  } while (false)

#endif // UTILS_H
