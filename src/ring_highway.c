/*
 * Copyright 2026 The RingDB Authors
 * Licensed under the AGPLv3 / SSPLv1 / RSALv2 Tri-License Framework
 */

#include "ring_highway.h"
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

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

// ============================================================================
// Phase 3: Full Async Highway Request/Response Pattern Implementation
// ============================================================================

// Global Request Trackers (one per core)
static request_tracker_t *request_trackers = NULL;

void highway_init_request_trackers(int total_cores) {
    request_trackers = calloc(total_cores, sizeof(request_tracker_t));
    
    for (int i = 0; i < total_cores; i++) {
        request_trackers[i].next_request_id = 0;
        atomic_store_explicit(&request_trackers[i].pending_count, 0, memory_order_relaxed);
        
        for (int j = 0; j < 256; j++) {
            request_trackers[i].states[j] = REQ_STATE_PENDING;
        }
    }
}

// Allocate a unique request ID for this core (auto-wrapping 0-255)
uint8_t highway_alloc_request_id(int source_core) {
    if (!request_trackers) return 0;
    
    uint8_t request_id = request_trackers[source_core].next_request_id++;
    return request_id;
}

// Send an async request through highway to remote core
int highway_send_request(int source_core, int target_core, highway_request_t *req) {
    if (!request_trackers) return 0;
    
    uint8_t request_id = req->request_id;
    
    // Store request in local tracker
    request_trackers[source_core].requests[request_id] = *req;
    request_trackers[source_core].states[request_id] = REQ_STATE_DISPATCHED;
    atomic_fetch_add_explicit(&request_trackers[source_core].pending_count, 1, memory_order_relaxed);
    
    // Create transport packet from request pointer
    ring_packet_t packet;
    packet.client_fd = source_core;  // Overload: source core ID
    packet.payload_ptr = (void *)req;
    
    // Send through highway
    return highway_push(source_core, target_core, packet);
}

// Process incoming highway requests on this core (call from event loop)
int highway_process_requests(int core_id) {
    if (!request_trackers) return 0;
    
    int processed = 0;
    ring_packet_t packet;
    
    // Check requests from all other cores
    for (int src = 0; src < core_count; src++) {
        if (src == core_id) continue;  // Skip self
        
        while (highway_pop(src, core_id, &packet) == 1) {
            highway_request_t *req = (highway_request_t *)packet.payload_ptr;
            
            // Mark as processing
            request_trackers[src].states[req->request_id] = REQ_STATE_PROCESSING;
            
            // TODO: Database operation would execute here
            // For now, mark operation as ready (stub implementation)
            // Real implementation would create and send highway_response_t
            
            request_trackers[src].states[req->request_id] = REQ_STATE_RESPONSE_READY;
            processed++;
        }
    }
    
    return processed;
}

// Collect responses from remote cores (call from event loop)
int highway_collect_responses(int core_id) {
    if (!request_trackers) return 0;
    
    int collected = 0;
    ring_packet_t packet;
    
    // Collect responses coming back from all other cores
    for (int src = 0; src < core_count; src++) {
        if (src == core_id) continue;  // Skip self
        
        while (highway_pop(src, core_id, &packet) == 1) {
            highway_response_t *resp = (highway_response_t *)packet.payload_ptr;
            
            if (resp->op_type == OP_HIGHWAY_RESPONSE) {
                uint8_t req_id = resp->request_id;
                
                // Mark request complete
                request_trackers[core_id].states[req_id] = REQ_STATE_COMPLETED;
                atomic_fetch_sub_explicit(&request_trackers[core_id].pending_count, 1, memory_order_relaxed);
                
                // Store response in response buffer if provided
                if (request_trackers[core_id].requests[req_id].response_buffer) {
                    *(highway_response_t *)request_trackers[core_id].requests[req_id].response_buffer = *resp;
                }
                
                collected++;
            }
        }
    }
    
    return collected;
}

// Check if a request has completed
bool highway_request_ready(int source_core, uint8_t request_id) {
    if (!request_trackers) return false;
    return request_trackers[source_core].states[request_id] == REQ_STATE_COMPLETED;
}

// Get the response from a completed request
highway_response_t* highway_get_response(int source_core, uint8_t request_id) {
    if (!request_trackers) return NULL;
    
    if (request_trackers[source_core].states[request_id] == REQ_STATE_COMPLETED) {
        // Response is stored in the response buffer
        char *response_buffer = request_trackers[source_core].requests[request_id].response_buffer;
        if (response_buffer) {
            return (highway_response_t *)response_buffer;
        }
    }
    
    return NULL;
}

// Check for timed-out requests (>5ms without response)
void highway_check_request_timeouts(int core_id, uint32_t current_time) {
    if (!request_trackers) return;
    
    const uint32_t TIMEOUT_MS = 5;  // 5ms timeout threshold
    
    for (int i = 0; i < 256; i++) {
        request_state_t state = request_trackers[core_id].states[i];
        
        if (state == REQ_STATE_DISPATCHED || state == REQ_STATE_PROCESSING) {
            uint32_t elapsed = current_time - request_trackers[core_id].requests[i].timestamp;
            
            if (elapsed > TIMEOUT_MS) {
                request_trackers[core_id].states[i] = REQ_STATE_TIMEOUT;
                atomic_fetch_sub_explicit(&request_trackers[core_id].pending_count, 1, memory_order_relaxed);
            }
        }
    }
}
