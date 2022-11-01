#include <assert.h>
#include <memory.h>

#include "base.h"

Arena arena_init(void* memory, u64 size) {
    Arena arena = {};

    arena.base = memory;
    arena.cursor = (u8*)memory;
    arena.end = arena.cursor + size;

    return arena;
}

void arena_clear(Arena* arena) {
    arena->cursor = (u8*)arena->base;
}

void* arena_push(Arena* arena, u64 size) {
    if (size == 0) {
        return NULL;
    }

    size = (size + 7) & ~7;

    assert((i64)size <= (arena->end - arena->cursor));
    void* ptr = arena->cursor;
    arena->cursor += size;
    return ptr;
}

void* arena_push_zero(Arena* arena, u64 size) {
    void* mem = arena_push(arena, size);
    if (mem) { memset(mem, 0, size); }
    return mem;
}

