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
#include "ring_highway.h" // Needed for cross-shard packet distribution

#define QUEUE_DEPTH 4096  // Depth of each individual core's submission ring
#define READ_BUF_SIZE 4096

// Tracking token identifiers for asynchronous completions
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

// Context arguments passed directly from thread instantiation in main.c
typedef struct {
    int core_id;
    int server_fd;
} worker_ctx_t;

// The asynchronous multi-shot reception loop executed per core
void* iouring_worker_loop(void* arg) {
    worker_ctx_t *ctx = (worker_ctx_t*)arg;
    int core_id = ctx->core_id;
    int server_fd = ctx->server_fd;

    struct io_uring ring;
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));

    // Force SQPOLL (Submission Queue Polling) to bypass system calls entirely if privileged
    // This allows a background kernel thread to poll our submissions natively
    params.flags |= IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 2000; // Keep kernel thread awake for 2000ms idle windows

    // Fall back to standard io_uring initialization if SQPOLL lacks system root permissions
    if (io_uring_queue_init_params(QUEUE_DEPTH, &ring, &params) < 0) {
        if (io_uring_queue_init(QUEUE_DEPTH, &ring, 0) < 0) {
            fprintf(stderr, "[-] Core [%d] Critical: io_uring initialization failed\n", core_id);
            pthread_exit(NULL);
        }
    }

    // Allocate persistent context tracker for our eternal Multi-Shot Accept ring command
    io_context_t *accept_ctx = malloc(sizeof(io_context_t));
    accept_ctx->fd = server_fd;
    accept_ctx->type = OP_MULTISHOT_ACCEPT;
    accept_ctx->core_id = core_id;

    // Secure an asynchronous Submission Queue Entry (SQE)
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    
    // Prepare multi-shot accept: Keeps accepting incoming client descriptors automatically
    io_uring_prep_multishot_accept(sqe, server_fd, NULL, NULL, 0);
    io_uring_sqe_set_data(sqe, accept_ctx);
    
    // Submit the command to the kernel ring memory pipeline
    io_uring_submit(&ring);

    struct io_uring_cqe *cqe;

    // Infinite asynchronous polling execution loop
    while (1) {
        // Put core worker to sleep efficiently until kernel processes a network transaction
        if (io_uring_wait_cqe(&ring, &cqe) < 0) continue;

        io_context_t *io_data = (io_context_t*)io_uring_cqe_get_data(cqe);

        if (cqe->res < 0) {
            // Filter common non-breaking notifications
            if (cqe->res == -EAGAIN || cqe->res == -EINTR) {
                io_uring_cqe_seen(&ring, cqe);
                continue;
            }
            fprintf(stderr, "[-] Core [%d] Async transaction failure: %s\n", core_id, strerror(-cqe->res));
            io_uring_cqe_seen(&ring, cqe);
            continue;
        }

        // Route actions based on active token types
        switch (io_data->type) {
            
            case OP_MULTISHOT_ACCEPT: {
                // In multi-shot accept mode, cqe->res holds the new connected client descriptor!
                int client_fd = cqe->res;

                // Provision a fresh tracking context for reading data from this new client connection
                io_context_t *read_ctx = malloc(sizeof(io_context_t));
                read_ctx->fd = client_fd;
                read_ctx->type = OP_READ;
                read_ctx->core_id = core_id;

                // Push an asynchronous read submission down the network ring for this client descriptor
                struct io_uring_sqe *read_sqe = io_uring_get_sqe(&ring);
                io_uring_prep_recv(read_sqe, client_fd, read_ctx->buffer, READ_BUF_SIZE, 0);
                io_uring_sqe_set_data(read_sqe, read_ctx);
                io_uring_submit(&ring);
                break;
            }

            case OP_READ: {
                int bytes_recv = cqe->res;

                if (bytes_recv == 0) {
                    // Clean connection termination from client side
                    close(io_data->fd);
                    free(io_data);
                } else {
                    // TODO: Pass the raw buffer directly to src/parser.c for Zero-Copy parsing!
                    // For now, let's echo a valid Redis status reply (+OK\r\n) back asynchronously
                    io_data->type = OP_WRITE;
                    snprintf(io_data->buffer, READ_BUF_SIZE, "+OK\r\n");

                    struct io_uring_sqe *write_sqe = io_uring_get_sqe(&ring);
                    io_uring_prep_send(write_sqe, io_data->fd, io_data->buffer, 5, 0);
                    io_uring_sqe_set_data(write_sqe, io_data);
                    io_uring_submit(&ring);
                }
                break;
            }

            case OP_WRITE: {
                // Write transaction complete. Recycle the descriptor context to listen for next query
                io_data->type = OP_READ;
                struct io_uring_sqe *recv_sqe = io_uring_get_sqe(&ring);
                io_uring_prep_recv(recv_sqe, io_data->fd, io_data->buffer, READ_BUF_SIZE, 0);
                io_uring_sqe_set_data(recv_sqe, io_data);
                io_uring_submit(&ring);
                break;
            }
        }

        // Notify kernel the completion entry is officially processed and reusable
        io_uring_cqe_seen(&ring, cqe);
    }
    return NULL;
}
