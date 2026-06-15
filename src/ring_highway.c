/*
 * Copyright 2026 The RingDB Authors
 * Licensed under the AGPLv3 / SSPLv1 / RSALv2 Tri-License Framework
 */

#include "ring_highway.h"
#include <stdlib.h>

// A 2D matrix array storing unidirectional pathways between all core permutations
// matrix[src][dest]
static spsc_ring_t **highway_matrix = NULL;
static int core_count = 0;

void highway_init_matrix(int total_cores) {
    core_count = total_cores;
    highway_matrix = malloc(sizeof(spsc_ring_t*) * total_cores);
    
    for (int i = 0; i < total_cores; i++) {
        highway_matrix[i] = calloc(total_cores, sizeof(spsc_ring_t));
        for (int j = 0; j < total_cores; j++) {
            atomic_store_explicit(&highway_matrix[i][j].head, 0, memory_order_relaxed);
            atomic_store_explicit(&highway_matrix[i][j].tail, 0, memory_order_relaxed);
        }
    }
}

int highway_push(int src_core, int dest_core, ring_packet_t packet) {
    spsc_ring_t *ring = &highway_matrix[src_core][dest_core];
    
    size_t current_head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    size_t current_tail = atomic_load_explicit(&ring->tail, memory_order_acquire);

    // Fast bounds boundary limit check (is ring full?)
    if ((current_head - current_tail) >= RING_SIZE) {
        return 0; // Bus is saturated. Producer must backoff and try on next cycle tick.
    }

    // Insert payload reference using bitwise AND mask instead of expensive division (%)
    ring->buffer[current_head & (RING_SIZE - 1)] = packet;

    // memory_order_release flushes cache lines to guarantee Consumer Core instantly views item
    atomic_store_explicit(&ring->head, current_head + 1, memory_order_release);
    return 1; // Handshake successful!
}

int highway_pop(int src_core, int dest_core, ring_packet_t *packet) {
    spsc_ring_t *ring = &highway_matrix[src_core][dest_core];
    
    size_t current_tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    size_t current_head = atomic_load_explicit(&ring->head, memory_order_acquire);

    // Check if channel is empty
    if (current_tail == current_head) {
        return 0; // No data waiting from remote core
    }

    // Extract item
    *packet = ring->buffer[current_tail & (RING_SIZE - 1)];

    // Advance tail, signaling to Producer that the slot is instantly reusable
    atomic_store_explicit(&ring->tail, current_tail + 1, memory_order_release);
    return 1; // Data captured!
}
