#pragma once

#include "common.h"

struct ResourcePool {
    u32 capacity;
    u64 stride;
    i32 free_count;
    u32* free_list;
    u32* generations;
    char data[1];
};

ResourcePool* resource_pool_new(Arena* arena, u32 capacity, u64 stride);

u64 resource_pool_alloc(ResourcePool* pool);
void resource_pool_free(ResourcePool* pool, u64 handle);

void* resource_pool_access_bytes(ResourcePool* pool, u64 handle);

u32 resource_pool_num_allocations(ResourcePool* pool);
bool resource_pool_handle_valid(ResourcePool* pool, u64 handle);

#define resource_pool_access(pool, handle, type) (assert(sizeof(type) == pool->stride), (type*)resource_pool_access_bytes(pool, handle))
