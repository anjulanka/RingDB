/*
 * Copyright 2026 The RingDB Authors
 * Licensed under the AGPLv3 / SSPLv1 / RSALv2 Tri-License Framework
 */

#ifndef RING_HIGHWAY_H
#define RING_HIGHWAY_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

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

// ============================================================================
// Phase 3: Full Async Highway Request/Response Pattern (OP_HIGHWAY_WAIT)
// ============================================================================

// Operation Type Definitions
typedef enum {
    OP_HIGHWAY_GET = 0x01,
    OP_HIGHWAY_SET = 0x02,
    OP_HIGHWAY_DEL = 0x03,
    OP_HIGHWAY_WAIT = 0x04,      // Async wait/response pattern
    OP_HIGHWAY_RESPONSE = 0x05    // Response packet
} highway_op_type_t;

// Result Status Codes
typedef enum {
    HS_PENDING = 0,
    HS_SUCCESS = 1,
    HS_FOUND = 2,
    HS_NOT_FOUND = 3,
    HS_ERROR = 4,
    HS_TIMEOUT = 5
} highway_status_t;

// Request Packet Structure (fits in highway transport)
typedef struct {
    highway_op_type_t op_type;      // Operation: GET/SET/DEL
    uint8_t request_id;              // Unique ID per request (0-255, wraps safely)
    uint8_t source_core;             // Which core initiated (0-7)
    uint8_t result_status;           // Status: PENDING/SUCCESS/NOT_FOUND/ERROR
    
    // Key Information
    uint16_t key_len;                // Length of key (max 65536 bytes)
    char *key_ptr;                   // Pointer to key in source arena
    
    // Value Information (for SET operations)
    uint16_t val_len;                // Length of value
    char *val_ptr;                   // Pointer to value in source arena
    
    // Metadata
    uint32_t timestamp;              // Request issue time (for timeout detection)
    char *response_buffer;           // Where to write response (in source core's arena)
    
} highway_request_t;

// Response Packet Structure (mirrors request for symmetry)
typedef struct {
    highway_op_type_t op_type;       // Always OP_HIGHWAY_RESPONSE
    uint8_t request_id;              // Echo back request_id for correlation
    uint8_t source_core;             // Original requesting core
    uint8_t result_status;           // FOUND/NOT_FOUND/SUCCESS/ERROR
    
    // Result Data
    uint16_t result_len;             // Length of result data
    char *result_ptr;                // Pointer to result in destination arena
    
    // Error Information
    char error_msg[64];              // Error description if failed
    
} highway_response_t;

// Request State Machine
typedef enum {
    REQ_STATE_PENDING = 0,
    REQ_STATE_DISPATCHED = 1,
    REQ_STATE_PROCESSING = 2,
    REQ_STATE_RESPONSE_READY = 3,
    REQ_STATE_COMPLETED = 4,
    REQ_STATE_TIMEOUT = 5
} request_state_t;

// Per-Core Request Tracking (one per core)
typedef struct {
    highway_request_t requests[256];           // Track all 256 possible in-flight IDs
    request_state_t states[256];               // State of each request
    uint8_t next_request_id;                   // Next ID to assign (auto-wrapping)
    _Atomic unsigned int pending_count;        // How many requests in flight
    
} request_tracker_t;

// Structural initialization and management functions
void highway_init_matrix(int total_cores);
int highway_push(int src_core, int dest_core, ring_packet_t packet);
int highway_pop(int src_core, int dest_core, ring_packet_t *packet);

// Phase 3: Async Request/Response Functions
void highway_init_request_trackers(int total_cores);
uint8_t highway_alloc_request_id(int source_core);
int highway_send_request(int source_core, int target_core, highway_request_t *req);
int highway_process_requests(int core_id);
int highway_collect_responses(int core_id);
bool highway_request_ready(int source_core, uint8_t request_id);
highway_response_t* highway_get_response(int source_core, uint8_t request_id);
void highway_check_request_timeouts(int core_id, uint32_t current_time);

#endif // RING_HIGHWAY_H
