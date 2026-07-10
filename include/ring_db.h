/*
 * Copyright 2026 The RingDB Authors
 * Licensed under the AGPLv3 / SSPLv1 / RSALv2 Tri-License Framework
 */

#ifndef RING_DB_H
#define RING_DB_H

#include "arena.h"
#include <stddef.h>

#define HASH_MAP_BUCKETS 262144 /* 256K buckets per shard; must be power of 2.
 *   Memory cost : 256K × 8B ptr = 2 MB per shard, 16 MB across 8 shards.
 *   At 1M keys  : ~3.8 keys/shard → avg chain 0.015  (near O(1)).
 *   At 10M keys : avg chain 0.15  (still excellent).
 *   Previous 65536 gave avg chain length ~1.9 at 1M keys, degrading towards O(n). */

// A single key-value structural record node inside our map bucket chains
typedef struct db_entry {
    char *key;              // Pointer to the key string allocated in the Arena
    char *value;            // Pointer to the value string allocated in the Arena
    size_t key_len;         // Exact length of the key
    size_t val_len;         // Exact length of the value
    struct db_entry *next;  // Pointer to handle hash collision links lineally
} db_entry_t;

// The main data dictionary array assigned to each core shard
typedef struct {
    db_entry_t *buckets[HASH_MAP_BUCKETS];
    arena_pool_t *arena;    // Link to this specific shard's pre-allocated memory pool tank
} shard_table_t;

// Storage lifecycle engine operations
shard_table_t* db_init_shard(void);
int db_set(shard_table_t *table, const char *key, size_t key_len, const char *val, size_t val_len);
db_entry_t* db_get(shard_table_t *table, const char *key, size_t key_len);
int db_del(shard_table_t *table, const char *key, size_t key_len);

/* NUM_CORES: must be a power of 2 (used as a bitmask for shard routing).
 * Override at cmake configure time: cmake -DNUM_CORES=64 ..
 * Default matches a typical 8-core workstation; set to 64 for c6gn.16xlarge. */
#ifndef NUM_CORES
#define NUM_CORES 8
#endif
_Static_assert((NUM_CORES & (NUM_CORES - 1)) == 0,
               "NUM_CORES must be a power of 2 for O(1) shard routing");
extern shard_table_t *shard_storage[NUM_CORES];

#endif // RING_DB_H
