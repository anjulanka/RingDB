/*
 * Copyright 2026 The RingDB Authors
 * Licensed under the AGPLv3 / SSPLv1 / RSALv2 Tri-License Framework
 */

#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>
#include <stdint.h>

// 1GB pre-allocated storage tank size per CPU core shard
#define ARENA_SHARD_SIZE (1024 * 1024 * 1024) 

typedef struct {
    uint8_t *buffer;    // Base pointer to the 1GB block of raw RAM
    size_t capacity;    // Total size (ARENA_SHARD_SIZE)
    size_t offset;      // Current writing head location
} arena_pool_t;

// Shard Memory Lifecycle Management Functions
arena_pool_t* arena_init_shard(void);
void* arena_alloc(arena_pool_t *pool, size_t size);
void arena_reset(arena_pool_t *pool);
void arena_destroy(arena_pool_t *pool);

#endif // ARENA_H
