/*
 * Copyright 2026 The RingDB Authors
 * Licensed under the AGPLv3 / SSPLv1 / RSALv2 Tri-License Framework
 */

#ifndef RING_HIGHWAY_H
#define RING_HIGHWAY_H

#include <stdatomic.h>
#include <stddef.h>

#define RING_SIZE 512 // Must remain a power of 2 for fast bitwise masking operations

// The 16-byte transport packet flying across core cache lines
typedef struct {
    int client_fd;       // Originating client connection socket descriptor
    void *payload_ptr;   // Direct reference pointer to parsed data or commands
} ring_packet_t;

// The Single-Producer Single-Consumer Lockless Cache Line Array
typedef struct {
    ring_packet_t buffer[RING_SIZE];
    _Atomic size_t head; // Modified exclusively by the Producer Core
    _Atomic size_t tail; // Modified exclusively by the Consumer Core
} spsc_ring_t;

// Structural initialization and management functions
void highway_init_matrix(int total_cores);
int highway_push(int src_core, int dest_core, ring_packet_t packet);
int highway_pop(int src_core, int dest_core, ring_packet_t *packet);

#endif // RING_HIGHWAY_H
