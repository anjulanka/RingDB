/*
 * Copyright 2026 The RingDB Authors
 * Licensed under the AGPLv3 / SSPLv1 / RSALv2 Tri-License Framework
 */

#include "ring_db.h"
#include "arena.h"
#include <string.h>
#include <stdlib.h>

// Lightning fast djb2 string hashing algorithm
static unsigned long calculate_hash(const char *str, size_t len) {
    unsigned long hash = 5381;
    for (size_t i = 0; i < len; i++) {
        hash = ((hash << 5) + hash) + str[i]; /* hash * 33 + c */
    }
    return hash;
}

shard_table_t* db_init_shard(void) {
    shard_table_t *table = malloc(sizeof(shard_table_t));
    if (!table) return NULL;

    // Initialize all bucket head pointers to NULL
    memset(table->buckets, 0, sizeof(table->buckets));

    // Tie this shard directly to its own private 1GB memory arena tank
    table->arena = arena_init_shard();
    if (!table->arena) {
        free(table);
        return NULL;
    }

    return table;
}

int db_set(shard_table_t *table, const char *key, size_t key_len, const char *val, size_t val_len) {
    unsigned long hash = calculate_hash(key, key_len);
    size_t index = hash & (HASH_MAP_BUCKETS - 1); // Fast bitwise AND masking instead of modulo division

    db_entry_t *entry = table->buckets[index];

    // Check if the key already exists to overwrite it
    while (entry != NULL) {
        if (entry->key_len == key_len && memcmp(entry->key, key, key_len) == 0) {
            // Carve out a fresh space in the private arena for the new value string segment
            char *new_val = arena_alloc(table->arena, val_len + 1);
            if (!new_val) return -1; // Out of memory block guardrail
            
            memcpy(new_val, val, val_len);
            new_val[val_len] = '\0';
            
            entry->value = new_val;
            entry->val_len = val_len;
            return 0; // Overwrite updated successfully
        }
        entry = entry->next;
    }

    // Key is brand new. Carve out structural blocks directly out of our Shard Arena tank!
    db_entry_t *new_entry = arena_alloc(table->arena, sizeof(db_entry_t));
    char *arena_key = arena_alloc(table->arena, key_len + 1);
    char *arena_val = arena_alloc(table->arena, val_len + 1);

    if (!new_entry || !arena_key || !arena_val) return -1;

    // Copy string byte lines securely into persistent arena blocks
    memcpy(arena_key, key, key_len);
    arena_key[key_len] = '\0';
    memcpy(arena_val, val, val_len);
    arena_val[val_len] = '\0';

    // Populate our structural node properties
    new_entry->key = arena_key;
    new_entry->key_len = key_len;
    new_entry->value = arena_val;
    new_entry->val_len = val_len;

    // Link the new node to the front of the bucket chain (Singly-Linked List insertion)
    new_entry->next = table->buckets[index];
    table->buckets[index] = new_entry;

    return 1; // Direct key-value record storage successful
}

db_entry_t* db_get(shard_table_t *table, const char *key, size_t key_len) {
    unsigned long hash = calculate_hash(key, key_len);
    size_t index = hash & (HASH_MAP_BUCKETS - 1);

    db_entry_t *entry = table->buckets[index];

    // Traverse the chain inside the target bucket slot
    while (entry != NULL) {
        if (entry->key_len == key_len && memcmp(entry->key, key, key_len) == 0) {
            return entry; // Node element found! Returns reference payload string safely
        }
        entry = entry->next;
    }

    return NULL; // Key record does not exist on this shard
}

int db_del(shard_table_t *table, const char *key, size_t key_len) {
    unsigned long hash = calculate_hash(key, key_len);
    size_t index = hash & (HASH_MAP_BUCKETS - 1);

    db_entry_t *entry = table->buckets[index];
    db_entry_t *prev = NULL;

    while (entry != NULL) {
        if (entry->key_len == key_len && memcmp(entry->key, key, key_len) == 0) {
            if (prev == NULL) {
                // Removing the head node of the bucket chain
                table->buckets[index] = entry->next;
            } else {
                // Snipping out a link in the middle or end of the chain list
                prev->next = entry->next;
            }
            // Note: In a pure sequential arena model, we don't call individual free() on the node.
            // The memory space stays occupied until arena_reset() clears the entire shard.
            return 1; // Node deleted successfully from index chain
        }
        prev = entry;
        entry = entry->next;
    }

    return 0; // Key did not exist
}
