/*
 * Copyright 2026 The RingDB Authors
 * Licensed under the AGPLv3 / SSPLv1 / RSALv2 Tri-License Framework
 */

#include "ring_highway.h"
#include "ring_db.h"
#include <stdlib.h>
#include <string.h>

static spsc_ring_t **highway_matrix = NULL;
static int core_count = 0;

// 🔥 FIX: Array of distinct cache-aligned pointer structures to completely eliminate False Sharing
static request_tracker_t *core_trackers[NUM_CORES] = {NULL};

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

    if ((current_head - current_tail) >= RING_SIZE) {
        return 0; 
    }

    ring->buffer[current_head & (RING_SIZE - 1)] = packet;
    atomic_store_explicit(&ring->head, current_head + 1, memory_order_release);
    return 1; 
}

int highway_pop(int src_core, int dest_core, ring_packet_t *packet) {
    spsc_ring_t *ring = &highway_matrix[src_core][dest_core];
    size_t current_tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    size_t current_head = atomic_load_explicit(&ring->head, memory_order_acquire);

    if (current_tail == current_head) {
        return 0; 
    }

    *packet = ring->buffer[current_tail & (RING_SIZE - 1)];
    atomic_store_explicit(&ring->tail, current_tail + 1, memory_order_release);
    return 1; 
}

// ============================================================================
// Phase 3: Full Async Highway Request/Response Pattern Implementation
// ============================================================================

void highway_init_request_trackers(int total_cores) {
    for (int i = 0; i < total_cores; i++) {
        // Force the OS to allocate the tracking frames aligned cleanly to 64-byte boundaries
        core_trackers[i] = aligned_alloc(64, sizeof(request_tracker_t));
        memset(core_trackers[i]->requests, 0, sizeof(highway_request_t) * 256);
        
        for (int j = 0; j < 256; j++) {
            atomic_store_explicit(&core_trackers[i]->states[j], REQ_STATE_COMPLETED, memory_order_relaxed);
        }
        core_trackers[i]->next_request_id = 0;
        atomic_store_explicit(&core_trackers[i]->pending_count, 0, memory_order_relaxed);
    }
}

int highway_alloc_request_id(int source_core) {
    request_tracker_t *tracker = core_trackers[source_core];
    uint8_t start_id = tracker->next_request_id;

    do {
        uint8_t test_id = tracker->next_request_id++;
        int expected_state = REQ_STATE_COMPLETED;
        
        // Use an explicit atomic Compare-And-Swap operation to securely claim a tracking slot
        if (atomic_compare_exchange_strong_explicit(
                &tracker->states[test_id], &expected_state, REQ_STATE_PENDING,
                memory_order_relaxed, memory_order_relaxed)) {
            return (int)test_id; 
        }
    } while (tracker->next_request_id != start_id);

    return -1; 
}

int highway_send_request(int source_core, int target_core, highway_request_t *req) {
    uint8_t req_id = req->request_id;
    request_tracker_t *tracker = core_trackers[source_core];

    tracker->requests[req_id] = *req;
    atomic_store_explicit(&tracker->states[req_id], REQ_STATE_DISPATCHED, memory_order_release);

    ring_packet_t packet;
    packet.client_fd = -1; // Overload tag: -1 flags this as an internal highway command pointer
    packet.payload_ptr = (void*)&tracker->requests[req_id];

    if (!highway_push(source_core, target_core, packet)) {
        atomic_store_explicit(&tracker->states[req_id], REQ_STATE_COMPLETED, memory_order_relaxed);
        return 0; 
    }

    atomic_fetch_add_explicit(&tracker->pending_count, 1, memory_order_relaxed);
    return 1; 
}

int highway_process_requests(int core_id) {
    int processed = 0;
    ring_packet_t packet;

    for (int src = 0; src < core_count; src++) {
        if (src == core_id) continue;

        while (highway_pop(src, core_id, &packet)) {
            processed++;
            highway_request_t *req = (highway_request_t*)packet.payload_ptr;
            
            // Execute lookups and writes using purely core-isolated variables
            if (req->op_type == OP_HIGHWAY_GET) {
                db_entry_t *entry = db_get(shard_storage[core_id], req->key_ptr, req->key_len);
                if (entry) {
                    req->result_status = HS_FOUND;
                    req->val_ptr = entry->value;
                    req->val_len = entry->val_len;
                } else {
                    req->result_status = HS_NOT_FOUND;
                }
            } 
            else if (req->op_type == OP_HIGHWAY_SET) {
                int res = db_set(shard_storage[core_id], req->key_ptr, req->key_len, req->val_ptr, req->val_len);
                req->result_status = (res >= 0) ? HS_SUCCESS : HS_ERROR;
            } 
            else if (req->op_type == OP_HIGHWAY_DEL) {
                int res = db_del(shard_storage[core_id], req->key_ptr, req->key_len);
                req->result_status = res ? HS_SUCCESS : HS_NOT_FOUND;
            }

            // Signal processing complete by updating the op_type token attribute
            req->op_type = OP_HIGHWAY_RESPONSE;

            ring_packet_t resp_packet = { .client_fd = -1, .payload_ptr = (void*)req };
            
            // Loop with hardware pause hint to prevent spinning up system bus temperatures if saturated
            while (!highway_push(core_id, src, resp_packet)) {
                __asm__ volatile ("pause");
            }
        }
    }
    return processed;
}

int highway_collect_responses(int core_id) {
    int collected = 0;
    ring_packet_t packet;

    for (int target = 0; target < core_count; target++) {
        if (target == core_id) continue;

        while (highway_pop(target, core_id, &packet)) {
            collected++;
            highway_request_t *req = (highway_request_t*)packet.payload_ptr;
            
            if (req->op_type == OP_HIGHWAY_RESPONSE) {
                request_tracker_t *tracker = core_trackers[core_id];
                // Update the state atomically so the main network loop instantly catches the response
                atomic_store_explicit(&tracker->states[req->request_id], REQ_STATE_RESPONSE_READY, memory_order_release);
                atomic_fetch_sub_explicit(&tracker->pending_count, 1, memory_order_relaxed);
            }
        }
    }
    return collected;
}

bool highway_request_ready(int source_core, uint8_t request_id) {
    int current_state = atomic_load_explicit(&core_trackers[source_core]->states[request_id], memory_order_acquire);
    return current_state == REQ_STATE_RESPONSE_READY;
}

highway_response_t* highway_get_response(int source_core, uint8_t request_id) {
    // Convert the tracking node safely back into a mirrored response structure block layout
    static _Thread_local highway_response_t static_resp;
    request_tracker_t *tracker = core_trackers[source_core];
    highway_request_t *req = &tracker->requests[request_id];

    static_resp.op_type = OP_HIGHWAY_RESPONSE;
    static_resp.request_id = request_id;
    static_resp.source_core = source_core;
    static_resp.result_status = req->result_status;
    static_resp.result_ptr = req->val_ptr;
    static_resp.result_len = req->val_len;

    // Release the tracker slot back to empty/usable state atomically
    atomic_store_explicit(&tracker->states[request_id], REQ_STATE_COMPLETED, memory_order_release);
    return &static_resp;
}

void highway_check_request_timeouts(int core_id, uint32_t current_time) {
    request_tracker_t *tracker = core_trackers[core_id];
    for (int i = 0; i < 256; i++) {
        int state = atomic_load_explicit(&tracker->states[i], memory_order_relaxed);
        if (state == REQ_STATE_DISPATCHED || state == REQ_STATE_PROCESSING) {
            if (current_time - tracker->requests[i].timestamp > 5000) {
                atomic_store_explicit(&tracker->states[i], REQ_STATE_TIMEOUT, memory_order_release);
                atomic_fetch_sub_explicit(&tracker->pending_count, 1, memory_order_relaxed);
            }
        }
    }
}

// 🔥 NEW HELPER: Safely returns the original network context pointer across module boundaries
void* highway_get_request_context(int source_core, uint8_t request_id) {
    if (core_trackers[source_core] == NULL) return NULL;
    return (void*)core_trackers[source_core]->requests[request_id].response_buffer;
}
