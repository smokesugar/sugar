#pragma once

#include "base.h"

void system_message_box(char* fmt, ...);
void debug_message(char* fmt, ...);

f32 engine_time();

struct Scratch {
    Arena* arena;
    u8* ptr;
};

Scratch get_scratch(Arena** conflicts, u32 conflict_count);
void release_scratch(Scratch scratch);

struct ReadFileResult {
    char* memory;
    u64 size;
};

ReadFileResult read_file(Arena* arena, char* path);
