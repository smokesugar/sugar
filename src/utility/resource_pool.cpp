#include <stddef.h>

#include "resource_pool.h"

ResourcePool* resource_pool_new(Arena* arena, u32 capacity, u64 stride) {
    ResourcePool* pool = (ResourcePool*)arena_push_zero(arena, offsetof(ResourcePool, data) + stride * capacity);

    pool->capacity = capacity;
    pool->stride = stride;

    pool->free_list = arena_push_array(arena, u32, capacity);
    pool->generations = arena_push_array(arena, u32, capacity);

    for (u32 i = 0; i < capacity; ++i) {
        pool->free_list[pool->free_count++] = i;
        pool->generations[i] = 1;
    }

    return pool;
}

u64 resource_pool_alloc(ResourcePool* pool) {
    assert(pool->free_count > 0);
    u32 index = pool->free_list[--pool->free_count];
    u32 generation = pool->generations[index];
    return ((u64)generation << 32) | index;
}

void resource_pool_free(ResourcePool* pool, u64 handle) {
    assert(resource_pool_handle_valid(pool, handle));
    u32 index = handle & UINT32_MAX;
    pool->free_list[pool->free_count++] = index;
    ++pool->generations[index];
}

void* resource_pool_access_bytes(ResourcePool* pool, u64 handle) {
    assert(resource_pool_handle_valid(pool, handle));
    u32 index = handle & UINT32_MAX;
    return (u8*)pool->data + index * pool->stride;
}

u32 resource_pool_num_allocations(ResourcePool* pool) {
    return pool->capacity - pool->free_count;
}

bool resource_pool_handle_valid(ResourcePool* pool, u64 handle) {
    u32 index = handle & UINT32_MAX;
    u32 generation = handle >> 32;
    return index < pool->capacity && generation == pool->generations[index];
}
