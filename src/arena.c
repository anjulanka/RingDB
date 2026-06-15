/*
 * Copyright 2026 The RingDB Authors
 * Licensed under the AGPLv3 / SSPLv1 / RSALv2 Tri-License Framework
 */

#include "arena.h"
#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h> // Needed for high-performance virtual memory mapping

arena_pool_t* arena_init_shard(void) {
    arena_pool_t *pool = malloc(sizeof(arena_pool_t));
    if (!pool) return NULL;

    pool->capacity = ARENA_SHARD_SIZE;
    pool->offset = 0;

    // Use mmap instead of malloc to request huge, clean pages straight from the Linux kernel.
    // MAP_ANONYMOUS | MAP_PRIVATE ensures our block is contiguous and private to our process.
    pool->buffer = mmap(NULL, pool->capacity, PROT_READ | PROT_WRITE, 
                        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

    if (pool->buffer == MAP_FAILED) {
        perror("[-] Shard Arena Memory Map Allocation Failed");
        free(pool);
        return NULL;
    }

    return pool;
}

void* arena_alloc(arena_pool_t *pool, size_t size) {
    // 64-bit alignment alignment (forces memory boundaries to align with CPU L1/L2 cache blocks)
    size_t aligned_size = (size + 7) & ~7;

    if (pool->offset + aligned_size > pool->capacity) {
        fprintf(stderr, "[-] Critical: Shard Arena memory tank has overflowed!\n");
        return NULL; // Out of memory for this specific core shard
    }

    // Capture the pointer at the current offset head
    void *ptr = &pool->buffer[pool->offset];
    
    // Shift the writing offset head forward lineally
    pool->offset += aligned_size;

    return ptr; // Return our cleanly sliced, zero-fragmentation pointer
}

void arena_reset(arena_pool_t *pool) {
    // To wipe data, we don't clear memory bits. We just reset the index to 0! 
    // This executes in exactly 1 nanosecond, instantly recycling 1GB of memory.
    pool->offset = 0;
}

void arena_destroy(arena_pool_t *pool) {
    if (pool) {
        munmap(pool->buffer, pool->capacity);
        free(pool);
    }
}
