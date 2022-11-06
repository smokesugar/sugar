#pragma once

#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t  i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

typedef i32 b32;

typedef float f32;
typedef double f64;

#define internal static
#define global_var static

#define ARRAY_LEN(x) (sizeof(x)/sizeof(x[0]))
#define UNUSED(x) ((void)x)

#define PI32 3.14159265359f

struct Arena {
    void* base;
    u8* cursor;
    u8* end;
};

Arena arena_init(void* memory, u64 size);
void arena_clear(Arena* arena);

void* arena_push(Arena* arena, u64 size);
void* arena_push_zero(Arena* arena, u64 size);

#define arena_push_array(arena, type, len) (type*)arena_push(arena, len * sizeof(type))
#define arena_push_array_zero(arena, type, len) (type*)arena_push_zero(arena, len * sizeof(type))

#define arena_push_struct(arena, type) arena_push_array(arena, type, 1)
#define arena_push_struct_zero(arena, type) arena_push_array_zero(arena, type, 1)

#define arena_mark(arena, type) ( assert(sizeof(type)%8==0), (type*)(arena->cursor) )
