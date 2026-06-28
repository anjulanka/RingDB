/*
 * Copyright 2026 The RingDB Authors
 * Licensed under the AGPLv3 / SSPLv1 / RSALv2 Tri-License Framework
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <liburing.h>
#include <sys/socket.h>
#include <pthread.h>

// Core RingDB Engine Header Imports
#include "ring_highway.h"
#include "parser.h"
#include "arena.h"
#include "ring_db.h"

#define QUEUE_DEPTH 4096  
#define READ_BUF_SIZE 4096
#define NUM_CORES 8 // Matches your hardware Ryzen 7 configuration setup

// Tracking tokens for our async completions
enum {
    OP_MULTISHOT_ACCEPT,
    OP_READ,
    OP_WRITE,
    OP_HIGHWAY_WAIT_STATE // ✅ Added to prevent thread execution blocking
};

typedef struct {
    int fd;
    int type;
    int core_id;
    uint8_t pending_req_id; // ✅ Added to map unique request slot IDs
    char buffer[READ_BUF_SIZE];
} io_context_t;

typedef struct {
    int core_id;
    int server_fd;
} worker_ctx_t;

// Isolated software shard memory array definitions (One per CPU core)
shard_table_t *shard_storage[NUM_CORES] = {NULL};

// ============================================================================
// 🌀 SHARED-NOTHING GET HANDLER (Local fast-path + Cross-core highway dispatch)
// ============================================================================
static void handle_async_get(int core_id, io_context_t *io_data, resp_command_t *cmd,
                              io_context_t **pending_io) {
    char *key = cmd->args[1].ptr;
    size_t key_len = cmd->args[1].len;

    unsigned long hash = 5381;
    for (size_t i = 0; i < key_len; i++) hash = ((hash << 5) + hash) + key[i];
    int target_shard = hash & (NUM_CORES - 1);

    if (target_shard == core_id) {
        // LOCAL FAST-PATH: Zero-copy direct read from our own shard
        db_entry_t *entry = db_get(shard_storage[core_id], key, key_len);
        if (entry)
            snprintf(io_data->buffer, READ_BUF_SIZE, "$%zu\r\n%s\r\n", entry->val_len, entry->value);
        else
            snprintf(io_data->buffer, READ_BUF_SIZE, "$-1\r\n");
        io_data->type = OP_WRITE;
    } else {
        // REMOTE PATH: Dispatch via SPSC lockless highway to the shard owner core
        int req_id = highway_alloc_request_id(core_id);
        if (req_id < 0) {
            snprintf(io_data->buffer, READ_BUF_SIZE, "-ERR Internal Pipeline Saturated\r\n");
            io_data->type = OP_WRITE;
            return;
        }

        static highway_request_t req;
        memset(&req, 0, sizeof(highway_request_t));
        req.op_type = OP_HIGHWAY_GET;
        req.request_id = (uint8_t)req_id;
        req.source_core = (uint8_t)core_id;
        req.result_status = HS_PENDING;
        req.key_ptr = key;
        req.key_len = (uint16_t)key_len;
        
        // ✅ FIX: Clear val_ptr and let the remote shard use it for the data value string!
        req.val_ptr = NULL; 
        req.val_len = 0;
        
        req.timestamp = (uint32_t)time(NULL);
        
        // ✅ FIX: Store the io_data pointer context inside the dedicated response_buffer field!
        req.response_buffer = (char *)io_data; 

        io_data->type = OP_HIGHWAY_WAIT_STATE;
        io_data->pending_req_id = (uint8_t)req_id;

         // ✅ Save your network context block directly into your tracking array before pushing!
        pending_io[req_id] = io_data; 

        if (!highway_send_request(core_id, target_shard, &req)) {
            pending_io[req_id] = NULL; // Clear tracking on failure
            snprintf(io_data->buffer, READ_BUF_SIZE, "-ERR Highway Saturated\r\n");
            io_data->type = OP_WRITE;
            return;
        }
        pending_io[req_id] = io_data;
    }
}

// ============================================================================
// 🧱 SHARED-NOTHING SET HANDLER (Local fast-path + Cross-core highway dispatch)
// ============================================================================
static void handle_async_set(int core_id, io_context_t *io_data, resp_command_t *cmd,
                              io_context_t **pending_io) {
    char *key = cmd->args[1].ptr;
    size_t key_len = cmd->args[1].len;
    char *val = cmd->args[2].ptr;
    size_t val_len = cmd->args[2].len;

    unsigned long hash = 5381;
    for (size_t i = 0; i < key_len; i++) hash = ((hash << 5) + hash) + key[i];
    int target_shard = hash & (NUM_CORES - 1);

    if (target_shard == core_id) {
        // LOCAL FAST-PATH: Write directly into our own arena
        int res = db_set(shard_storage[core_id], key, key_len, val, val_len);
        snprintf(io_data->buffer, READ_BUF_SIZE, res >= 0 ? "+OK\r\n" : "-ERR Storage Arena Saturated\r\n");
        io_data->type = OP_WRITE;
    } else {
        // REMOTE PATH: Route write via SPSC lockless highway to the shard owner core
        int req_id = highway_alloc_request_id(core_id);
        if (req_id < 0) {
            snprintf(io_data->buffer, READ_BUF_SIZE, "-ERR Internal Pipeline Saturated\r\n");
            io_data->type = OP_WRITE;
            return;
        }

        static  highway_request_t req = {0};
        memset(&req, 0, sizeof(highway_request_t));

        req.op_type         = OP_HIGHWAY_SET;
        req.request_id      = (uint8_t)req_id;
        req.source_core     = (uint8_t)core_id;
        req.result_status   = HS_PENDING;
        req.key_ptr         = key;
        req.key_len         = (uint16_t)key_len;
        req.val_ptr         = val;
        req.val_len         = (uint16_t)val_len;
        req.timestamp       = (uint32_t)time(NULL);
        req.response_buffer = (char *)io_data;  // carried back on the return lane

        io_data->type = OP_HIGHWAY_WAIT_STATE;
        io_data->pending_req_id = (uint8_t)req_id;

        if (!highway_send_request(core_id, target_shard, &req)) {
            snprintf(io_data->buffer, READ_BUF_SIZE, "-ERR Highway Saturated\r\n");
            io_data->type = OP_WRITE;
            return;
        }
        pending_io[req_id] = io_data;
    }
}



void* iouring_worker_loop(void* arg) {
    worker_ctx_t *ctx = (worker_ctx_t*)arg;
    int core_id = ctx->core_id;
    int server_fd = ctx->server_fd;

    struct io_uring ring;
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));

    // 1. Core Memory Isolation (Instantiate our private non-locked storage shard)
    if (shard_storage[core_id] == NULL) {
        shard_storage[core_id] = db_init_shard();
        if (!shard_storage[core_id]) {
            fprintf(stderr, "[-] Core [%d] Critical: Shard allocation failed\n", core_id);
            pthread_exit(NULL);
        }
    }

    // 2. Performance Tuning (Attempt to pass network handling to a kernel loop thread)
    params.flags |= IORING_SETUP_SQPOLL;
    // ✅ FIX 5: Add IORING_SETUP_SINGLE_ISSUER for reduced SQ contention
    #ifdef IORING_SETUP_SINGLE_ISSUER
    params.flags |= IORING_SETUP_SINGLE_ISSUER;
    #endif
    params.sq_thread_idle = 2000; 

    if (io_uring_queue_init_params(QUEUE_DEPTH, &ring, &params) < 0) {
        // Fall back gracefully to standard io_uring if system lacks root permissions
        if (io_uring_queue_init(QUEUE_DEPTH, &ring, 0) < 0) {
            fprintf(stderr, "[-] Core [%d] Critical: io_uring setup failed\n", core_id);
            pthread_exit(NULL);
        }
    }

    // 3. Initiate Asynchronous Multi-Shot Connection Capture
    io_context_t *accept_ctx = malloc(sizeof(io_context_t));
    accept_ctx->fd = server_fd;
    accept_ctx->type = OP_MULTISHOT_ACCEPT;
    accept_ctx->core_id = core_id;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_multishot_accept(sqe, server_fd, NULL, NULL, 0);
    io_uring_sqe_set_data(sqe, accept_ctx);
    io_uring_submit(&ring);

    // Per-worker table: maps request_id → waiting io_context for highway response dispatch
    io_context_t *pending_io[256];
    memset(pending_io, 0, sizeof(pending_io));

    struct io_uring_cqe *cqe;

    // 4. Shared-Nothing Execution Loop: highway polling + non-blocking network events every tick
    while (1) {
        // Step A: Execute any GET/SET requests that other cores have dispatched to our shard
        highway_process_requests(core_id);

        // Step B: Collect completed responses for requests we dispatched to other cores
        highway_collect_responses(core_id);

        // Step C: For every ready response, build the RESP reply and fire the io_uring send
        for (int i = 0; i < 256; i++) {
            if (!pending_io[i]) continue;
            if (!highway_request_ready(core_id, (uint8_t)i)) continue;

            io_context_t *resolved = pending_io[i];
            pending_io[i] = NULL;
            highway_response_t *resp = highway_get_response(core_id, (uint8_t)i);

            if (resp->result_status == HS_FOUND) {
                 // Fix your format string to match RESP specifications exactly
            snprintf(resolved->buffer, READ_BUF_SIZE, "$%zu\r\n%s\r\n", 
                     (size_t)resp->result_len, resp->result_ptr);
            } else if (resp->result_status == HS_NOT_FOUND) {
                snprintf(resolved->buffer, READ_BUF_SIZE, "$-1\r\n");
            } else if (resp->result_status == HS_SUCCESS) {
                snprintf(resolved->buffer, READ_BUF_SIZE, "+OK\r\n");
            } else {
                snprintf(resolved->buffer, READ_BUF_SIZE, "-ERR Highway op failed\r\n");
            }
            resolved->type = OP_WRITE;      

            struct io_uring_sqe *resp_sqe = io_uring_get_sqe(&ring);
            if (resp_sqe) {
                io_uring_prep_send(resp_sqe, resolved->fd, resolved->buffer,
                                   strlen(resolved->buffer), 0);
                io_uring_sqe_set_data(resp_sqe, resolved);
                io_uring_submit(&ring);
            }
        }

        // Step D: Non-blocking check for new network events (busy-poll for ultra-low latency)
        if (io_uring_peek_cqe(&ring, &cqe) != 0) continue;

        io_context_t *io_data = (io_context_t*)io_uring_cqe_get_data(cqe);

        if (cqe->res < 0) {
            io_uring_cqe_seen(&ring, cqe);
            continue;
        }

        switch (io_data->type) {
            
            case OP_MULTISHOT_ACCEPT: {
                int client_fd = cqe->res;

                io_context_t *read_ctx = malloc(sizeof(io_context_t));
                read_ctx->fd = client_fd;
                read_ctx->type = OP_READ;
                read_ctx->core_id = core_id;

                struct io_uring_sqe *read_sqe = io_uring_get_sqe(&ring);
                io_uring_prep_recv(read_sqe, client_fd, read_ctx->buffer, READ_BUF_SIZE, 0);
                io_uring_sqe_set_data(read_sqe, read_ctx);
                io_uring_submit(&ring);
                break;
            }

            case OP_READ: {
                int bytes_recv = cqe->res;

                if (bytes_recv <= 0) {
                    close(io_data->fd);
                    free(io_data);
                } else {
                    resp_command_t cmd;
                    
                    // Run our Zero-Copy RESP Parser directly over the socket buffer line
                    if (resp_parse_buffer(io_data->buffer, bytes_recv, &cmd) < 0) {
                        snprintf(io_data->buffer, READ_BUF_SIZE, "-ERR Protocol Syntax Error\r\n");
                        io_data->type = OP_WRITE;
                    } 
                    else {
                        // Routing evaluation using our ultra-fast inline integer IDs
                        if (cmd.command_id == CMD_GET) {
                            handle_async_get(core_id, io_data, &cmd, pending_io);
                        }
                        else if (cmd.command_id == CMD_MGET) {
                            // ─────────────────────────────────────────────────────────────
                            // 🌀 SCATTER-GATHER MULTI-KEY READ DATA PATH
                            // ─────────────────────────────────────────────────────────────
                            if (cmd.arg_count < 2) {
                                snprintf(io_data->buffer, READ_BUF_SIZE, "-ERR wrong number of arguments for 'mget' command\r\n");
                                io_data->type = OP_WRITE;
                            } else {
                                // Start building a standard RESP Array response (e.g., *3\r\n)
                                size_t offset = snprintf(io_data->buffer, READ_BUF_SIZE, "*%d\r\n", cmd.arg_count - 1);
                                
                                // Loop through all requested keys (skipping index 0 which is the "MGET" verb)
                                for (int i = 1; i < cmd.arg_count; i++) {
                                    char *key = cmd.args[i].ptr;
                                    size_t key_len = cmd.args[i].len;

                                    // 🔑 KEY-BASED SHARDING: Route to correct shard based on key hash
                                    unsigned long hash = 5381;
                                    for (size_t j = 0; j < key_len; j++) {
                                        hash = ((hash << 5) + hash) + key[j];
                                    }
                                    int target_shard = hash & (NUM_CORES - 1);

                                    // Fetch from the correct shard (which may be any core, not just the current one)
                                    db_entry_t *entry = db_get(shard_storage[target_shard], key, key_len);
                                    if (entry) {
                                        offset += snprintf(io_data->buffer + offset, READ_BUF_SIZE - offset, 
                                                           "$%zu\r\n%s\r\n", entry->val_len, entry->value);
                                    } else {
                                        offset += snprintf(io_data->buffer + offset, READ_BUF_SIZE - offset, "$-1\r\n");
                                    }
                                }
                                io_data->type = OP_WRITE;
                            }
                        }
                        else if (cmd.command_id == CMD_SET) {
                            handle_async_set(core_id, io_data, &cmd, pending_io);
                        } 
                        else if (cmd.command_id == CMD_PING) {
                            snprintf(io_data->buffer, READ_BUF_SIZE, "+PONG\r\n");
                            io_data->type = OP_WRITE;
                        } 
                        else {
                            snprintf(io_data->buffer, READ_BUF_SIZE, "-ERR Unknown Command Received\r\n");
                            io_data->type = OP_WRITE;
                        }
                    }

                    // Push out our asynchronous non-blocking network response
                    if (io_data->type == OP_WRITE) {
                        struct io_uring_sqe *write_sqe = io_uring_get_sqe(&ring);
                        // ✅ FIX 4: Add MSG_ZEROCOPY for zero-copy sends (kernel 5.19+)
                        int send_flags = 0;
                        #ifdef MSG_ZEROCOPY
                        send_flags = MSG_ZEROCOPY;
                        #endif
                        io_uring_prep_send(write_sqe, io_data->fd, io_data->buffer, strlen(io_data->buffer), send_flags);
                        io_uring_sqe_set_data(write_sqe, io_data);
                        io_uring_submit(&ring);
                    }
                }
                break;
            }

            case OP_WRITE: {
                // Return pipeline complete. Recycle context block to capture next network command
                io_data->type = OP_READ;
                struct io_uring_sqe *recv_sqe = io_uring_get_sqe(&ring);
                io_uring_prep_recv(recv_sqe, io_data->fd, io_data->buffer, READ_BUF_SIZE, 0);
                io_uring_sqe_set_data(recv_sqe, io_data);
                io_uring_submit(&ring);
                break;
            }

            case OP_HIGHWAY_WAIT_STATE:
                // Response arrives via the highway polling loop above; no io_uring action needed.
                break;
        }

        io_uring_cqe_seen(&ring, cqe);
    }
    return NULL;
}
