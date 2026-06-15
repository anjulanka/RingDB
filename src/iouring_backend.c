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
    OP_WRITE
};

typedef struct {
    int fd;
    int type;
    int core_id;
    char buffer[READ_BUF_SIZE];
} io_context_t;

typedef struct {
    int core_id;
    int server_fd;
} worker_ctx_t;

// Isolated software shard memory array definitions (One per CPU core)
static shard_table_t *shard_storage[NUM_CORES] = {NULL};

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

    struct io_uring_cqe *cqe;

    // 4. Pure Asynchronous Execution Loop
    while (1) {
        if (io_uring_wait_cqe(&ring, &cqe) < 0) continue;

        io_context_t *io_data = (io_context_t*)io_uring_cqe_get_data(cqe);

        if (cqe->res < 0) {
            if (cqe->res == -EAGAIN || cqe->res == -EINTR) {
                io_uring_cqe_seen(&ring, cqe);
                continue;
            }
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
                            // ─────────────────────────────────────────────────────────────
                            // ⚡ PURE ZERO-COPY READ DATA PATH
                            // Key variable references the socket packet stream directly
                            // ─────────────────────────────────────────────────────────────
                            char *key = cmd.args[1].ptr;
                            size_t key_len = cmd.args[1].len;

                            db_entry_t *entry = db_get(shard_storage[core_id], key, key_len);
                            
                            if (entry) {
                                snprintf(io_data->buffer, READ_BUF_SIZE, "$%zu\r\n%s\r\n", entry->val_len, entry->value);
                            } else {
                                snprintf(io_data->buffer, READ_BUF_SIZE, "$-1\r\n"); // Standard Redis Null reply
                            }
                            io_data->type = OP_WRITE;
                        } 
                        else if (cmd.command_id == CMD_SET) {
                            // ─────────────────────────────────────────────────────────────
                            // 🧱 MEMORY ARENA WRITE DATA PATH
                            // Strings copy out of socket line into persistent RAM blocks
                            // ─────────────────────────────────────────────────────────────
                            char *key = cmd.args[1].ptr;
                            size_t key_len = cmd.args[1].len;
                            char *val = cmd.args[2].ptr;
                            size_t val_len = cmd.args[2].len;

                            // db_set carves out sequential space within our 1GB pre-allocated pool tank
                            int res = db_set(shard_storage[core_id], key, key_len, val, val_len);
                            
                            if (res >= 0) {
                                snprintf(io_data->buffer, READ_BUF_SIZE, "+OK\r\n");
                            } else {
                                snprintf(io_data->buffer, READ_BUF_SIZE, "-ERR Storage Arena Saturated\r\n");
                            }
                            io_data->type = OP_WRITE;
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
                    struct io_uring_sqe *write_sqe = io_uring_get_sqe(&ring);
                    io_uring_prep_send(write_sqe, io_data->fd, io_data->buffer, strlen(io_data->buffer), 0);
                    io_uring_sqe_set_data(write_sqe, io_data);
                    io_uring_submit(&ring);
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
        }

        io_uring_cqe_seen(&ring, cqe);
    }
    return NULL;
}
