/*
 * Copyright 2026 The RingDB Authors
 * Licensed under the AGPLv3 / SSPLv1 / RSALv2 Tri-License Framework
 */

#include "ring_db.h"
#include "arena.h"
#include <string.h>
#include <stdlib.h>

// Lightning-fast djb2 string hashing algorithm
static unsigned long calculate_hash(const char *str, size_t len) {
    unsigned long hash = 5381;
    for (size_t i = 0; i < len; i++) {
        hash = ((hash << 5) + hash) + str[i];
    }
    return hash;
}

shard_table_t* db_init_shard(void) {
    shard_table_t *table = malloc(sizeof(shard_table_t));
    if (!table) return NULL;

    memset(table->buckets, 0, sizeof(table->buckets));

    table->arena = arena_init_shard();
    if (!table->arena) {
        free(table);
        return NULL;
    }

    return table;
}

int db_set(shard_table_t *table, const char *key, size_t key_len, const char *val, size_t val_len) {
    unsigned long hash = calculate_hash(key, key_len);
    size_t index = hash & (HASH_MAP_BUCKETS - 1);

    db_entry_t *entry = table->buckets[index];

    // 1. Check if the key already exists to overwrite it cleanly
    while (entry != NULL) {
        if (entry->key_len == key_len && memcmp(entry->key, key, key_len) == 0) {
            // 🔥 FIXED: Use the local parameter 'val_len', completely free of network packet structures
            char *new_val = arena_alloc(table->arena, val_len + 1);
            if (!new_val) return -1;
            
            memcpy(new_val, val, val_len);
            new_val[val_len] = '\0';
            
            entry->value = new_val;
            entry->val_len = val_len;
            return 0; 
        }
        entry = entry->next;
    }

    // 2. Key is brand new. Carve storage frames directly out of our pre-allocated RAM Arena
    db_entry_t *new_entry = arena_alloc(table->arena, sizeof(db_entry_t));
    char *arena_key = arena_alloc(table->arena, key_len + 1);
    char *arena_val = arena_alloc(table->arena, val_len + 1);

    if (!new_entry || !arena_key || !arena_val) return -1;

    // 🔥 FIXED: Uses the clean local 'key' and 'val' function variables directly!
    memcpy(arena_key, key, key_len);
    arena_key[key_len] = '\0';
    
    memcpy(arena_val, val, val_len);
    arena_val[val_len] = '\0';

    new_entry->key = arena_key;
    new_entry->key_len = key_len;
    new_entry->value = arena_val;
    new_entry->val_len = val_len;

    // Link the new record node to the front of the bucket index chain
    new_entry->next = table->buckets[index];
    table->buckets[index] = new_entry;

    return 1; 
}

db_entry_t* db_get(shard_table_t *table, const char *key, size_t key_len) {
    unsigned long hash = calculate_hash(key, key_len);
    size_t index = hash & (HASH_MAP_BUCKETS - 1);

    db_entry_t *entry = table->buckets[index];

    while (entry != NULL) {
        if (entry->key_len == key_len && memcmp(entry->key, key, key_len) == 0) {
            return entry; 
        }
        entry = entry->next;
    }

    return NULL; 
}

int db_del(shard_table_t *table, const char *key, size_t key_len) {
    unsigned long hash = calculate_hash(key, key_len);
    size_t index = hash & (HASH_MAP_BUCKETS - 1);

    db_entry_t *entry = table->buckets[index];
    db_entry_t *prev = NULL;

    while (entry != NULL) {
        if (entry->key_len == key_len && memcmp(entry->key, key, key_len) == 0) {
            if (prev == NULL) {
                table->buckets[index] = entry->next;
            } else {
                prev->next = entry->next;
            }
            return 1; 
        }
        prev = entry;
        entry = entry->next;
    }

    return 0; 
}