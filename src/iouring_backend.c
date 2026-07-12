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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>

// Core RingDB Engine Header Imports
#include "ring_highway.h"
#include "parser.h"
#include "arena.h"
#include "ring_db.h"

/* io_uring queue depth: SQ and CQ ring size per worker core.
 * 8192 entries covers pipeline=16 × 512 concurrent connections with headroom.
 * Raising beyond 16384 yields diminishing returns and increases kernel memory. */
#define QUEUE_DEPTH 8192

/* Per-connection recv buffer.  Must be large enough to hold a full pipeline
 * burst in one recv call.  Worst case: pipeline=16 × (header ~50B + value 512B)
 * ≈ 8.8 KB.  64 KB gives comfortable headroom for any realistic workload and
 * keeps each io_context_t well below 128 KB (acceptable per-connection cost). */
#define READ_BUF_SIZE 65536
#ifndef NUM_CORES
#define NUM_CORES 8 // Default fallback matches your hardware Ryzen 7 configuration setup
#endif

// Tracking tokens for our async completions
enum {
    OP_MULTISHOT_ACCEPT,
    OP_READ,
    OP_WRITE,
    OP_MULTISHOT_READ,
    OP_HIGHWAY_WAIT_STATE // ✅ Added to prevent thread execution blocking
};

typedef struct {
    int fd;
    int type;
    int core_id;
    int mget_total_keys;
    int mget_responses_received;
    uint8_t mget_req_ids[MAX_RESP_ARGS];
    size_t mget_response_offset;
    char buffer[READ_BUF_SIZE];
} io_context_t;

typedef struct {
    int core_id;
    int server_fd;
} worker_ctx_t;

// Isolated software shard memory array definitions (One per CPU core)
shard_table_t *shard_storage[NUM_CORES] = {NULL};

// ============================================================================
// 🌀 SHARED-NOTHING MGET HANDLER (Scatter-Gather via Highways)
// ============================================================================
static void handle_async_mget(int core_id, io_context_t *io_data, resp_command_t *cmd,
                              io_context_t **pending_io, int *mget_pending) {
    if (cmd->arg_count < 2) {
        snprintf(io_data->buffer, READ_BUF_SIZE, "-ERR wrong number of arguments for 'mget' command\r\n");
        io_data->type = OP_WRITE;
        return;
    }

    int num_keys = cmd->arg_count - 1;
    io_data->type = OP_MULTISHOT_READ;
    io_data->mget_total_keys = num_keys;
    io_data->mget_responses_received = 0;
    io_data->mget_response_offset = snprintf(io_data->buffer, READ_BUF_SIZE, "*%d\r\n", num_keys);

    for (int i = 0; i < num_keys; i++) {
        char *key = cmd->args[i + 1].ptr;
        size_t key_len = cmd->args[i + 1].len;

        unsigned long hash = 5381;
        for (size_t j = 0; j < key_len; j++) hash = ((hash << 5) + hash) + key[j];
        int target_shard = hash & (NUM_CORES - 1);

        if (target_shard == core_id) {
            // LOCAL MGET KEY: Read directly from our own shard
            db_entry_t *entry = db_get(shard_storage[core_id], key, key_len);
            if (entry) {
                io_data->mget_response_offset += snprintf(io_data->buffer + io_data->mget_response_offset,
                                                          READ_BUF_SIZE - io_data->mget_response_offset,
                                                          "$%zu\r\n%s\r\n", entry->val_len, entry->value);
            } else {
                io_data->mget_response_offset += snprintf(io_data->buffer + io_data->mget_response_offset,
                                                          READ_BUF_SIZE - io_data->mget_response_offset, "$-1\r\n");
            }
            io_data->mget_responses_received++;
        } else {
            // REMOTE MGET KEY: Dispatch via SPSC lockless highway
            int req_id = highway_alloc_request_id(core_id);
            if (req_id < 0) {
                // If we can't allocate a request, we have to abort the MGET
                // A more robust implementation might queue the request or retry
                io_data->mget_response_offset += snprintf(io_data->buffer + io_data->mget_response_offset,
                                                          READ_BUF_SIZE - io_data->mget_response_offset, "$-1\r\n");
                io_data->mget_responses_received++;
                continue;
            }

            io_data->mget_req_ids[i] = (uint8_t)req_id;

            highway_request_t req = {0};
            req.op_type = OP_HIGHWAY_GET;
            req.request_id = (uint8_t)req_id;
            req.source_core = (uint8_t)core_id;
            req.key_ptr = key;
            req.key_len = (uint16_t)key_len;

            pending_io[req_id] = io_data;
            if (!highway_send_request(core_id, target_shard, &req)) {
                pending_io[req_id] = NULL;
                io_data->mget_response_offset += snprintf(io_data->buffer + io_data->mget_response_offset,
                                                          READ_BUF_SIZE - io_data->mget_response_offset, "$-1\r\n");
                io_data->mget_responses_received++;
            } else {
                (*mget_pending)++;
            }
        }
    }

    // If all keys were local, we can send the response immediately
    if (io_data->mget_responses_received == io_data->mget_total_keys) {
        io_data->type = OP_WRITE;
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
    params.sq_thread_idle = 2000; /* ms: SQPOLL thread parks after 10 ms idle.
                                  * Under load it stays perpetually awake.
                                  * 2000 ms (old default) wasted a full CPU
                                  * core for 2 s after every quiet period. */

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

    // Per-worker table: maps request_id → waiting io_context for MGET scatter-gather
    io_context_t *pending_io[256];
    memset(pending_io, 0, sizeof(pending_io));
    /* O(1) guard: skip Step B entirely when no MGET sub-requests are in flight */
    int pending_io_count = 0;

    struct io_uring_cqe *cqe;

    // 4. Shared-Nothing Execution Loop: highway polling + non-blocking network events every tick
    while (1) {
        // Step A: Process inbound highway packets (requests and responses)
        highway_process_requests(core_id);

        // Step B: For every ready MGET response, build the RESP reply and fire the io_uring send
        if (pending_io_count > 0)
        for (int i = 0; i < 256; i++) {
            if (!pending_io[i]) continue;
            if (!highway_request_ready(core_id, (uint8_t)i)) continue;

            io_context_t *resolved = pending_io[i];
            highway_response_t *resp = highway_get_response(core_id, (uint8_t)i);

            if (resolved->type == OP_MULTISHOT_READ) {
                // This is a response for an MGET sub-request
                if (resp->result_status == HS_FOUND) {
                    resolved->mget_response_offset += snprintf(resolved->buffer + resolved->mget_response_offset,
                                                               READ_BUF_SIZE - resolved->mget_response_offset,
                                                               "$%zu\r\n%s\r\n", (size_t)resp->result_len, resp->result_ptr);
                } else {
                    resolved->mget_response_offset += snprintf(resolved->buffer + resolved->mget_response_offset,
                                                               READ_BUF_SIZE - resolved->mget_response_offset, "$-1\r\n");
                }
                resolved->mget_responses_received++;
                pending_io[i] = NULL;
                pending_io_count--;

                if (resolved->mget_responses_received == resolved->mget_total_keys) {
                    resolved->type = OP_WRITE;
                    struct io_uring_sqe *resp_sqe = io_uring_get_sqe(&ring);
                    if (resp_sqe) {
                        io_uring_prep_send(resp_sqe, resolved->fd, resolved->buffer,
                                           strlen(resolved->buffer), 0);
                        io_uring_sqe_set_data(resp_sqe, resolved);
                        io_uring_submit(&ring);
                    }
                }
                continue;
            }
            
            pending_io[i] = NULL;
            pending_io_count--;

            if (resp->result_status == HS_FOUND) {
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

        // ============================================================================
        // Step C: Adaptive Threshold Fallback (Yields CPU to awaken SQPOLL thread)
        // ============================================================================
        int peek_ret = io_uring_peek_cqe(&ring, &cqe);

        if (peek_ret != 0) {
            // Completed ring is empty. Track spins before falling back to kernel sleep.
            static __thread uint64_t spin_count = 0;
            spin_count++;

            if (spin_count < 4096) {
                // High-velocity burst path: hint the CPU to yield its hyperthread execution slot
                #if defined(__x86_64__)
                    __asm__ __volatile__("pause" ::: "memory");
                #endif
                continue;
            }

            // Slow path: Line went quiet. Perform a blocking wait to let SQPOLL run unthrottled
            spin_count = 0;
            int wait_ret = io_uring_wait_cqe(&ring, &cqe);
            if (wait_ret < 0) {
                continue;
            }
        } else {
            // Network hit registered: Reset our adaptive tracking counter
            static __thread uint64_t spin_count = 0;
            spin_count = 0;
        }

        io_context_t *io_data = (io_context_t*)io_uring_cqe_get_data(cqe);

        if (cqe->res < 0) {
            io_uring_cqe_seen(&ring, cqe);
            continue;
        }

        switch (io_data->type) {
            
            case OP_MULTISHOT_ACCEPT: {
                int client_fd = cqe->res;

                /* Disable Nagle on the accepted socket so small responses
                 * are sent immediately without waiting for ACK. */
                int nodelay = 1;
                setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY,
                           &nodelay, sizeof(nodelay));

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
                    break;
                }

                /* ══ Pipeline Phase 1: Parse + Execute local / Dispatch highway ══════════
                 * Process every command in the recv buffer in one forward pass.
                 *
                 * LOCAL commands execute inline — zero extra latency.
                 * REMOTE commands are dispatched to the target shard’s SPSC
                 *   highway lane immediately, WITHOUT blocking.  All N remote
                 *   requests for the entire pipeline burst are in-flight
                 *   simultaneously by the time Phase 1 finishes.
                 *
                 * MGET uses the existing async scatter-gather path (handled
                 *   after the main loop via mget_fallback).
                 * ════════════════════════════════════════════════════════════════════ */
                typedef struct {
                    bool    is_highway;   /* awaiting highway response      */
                    bool    is_set;       /* false=GET, true=SET            */
                    uint8_t req_id;       /* highway slot (is_highway only) */
                    /* Result — filled Phase 1 (local) or Phase 2 (highway)  */
                    bool    found;        /* GET: key present               */
                    bool    success;      /* SET: write succeeded           */
                    const char *val_ptr;  /* GET: stable arena pointer      */
                    size_t  val_len;      /* GET: value byte count          */
                    /* PING / error / saturation messages go here            */
                    char    resp[48];
                    int     resp_len;     /* >0 ⇒ resp[] is the final reply  */
                } cmd_slot_t;

                cmd_slot_t slots[MAX_RESP_ARGS];
                int num_slots   = 0;
                int num_highway = 0;   /* highway requests still in flight  */
                int parse_off   = 0;
                bool mget_fallback = false;

                while (parse_off < bytes_recv && num_slots < MAX_RESP_ARGS) {
                    resp_command_t cmd;
                    int consumed = resp_parse_buffer(io_data->buffer + parse_off,
                                                     bytes_recv - parse_off, &cmd);
                    if (consumed < 0) {
                        cmd_slot_t *s = &slots[num_slots++];
                        memset(s, 0, sizeof(*s));
                        s->resp_len = snprintf(s->resp, sizeof(s->resp),
                                               "-ERR Protocol Syntax Error\r\n");
                        break;
                    }
                    parse_off += consumed;

                    if (cmd.command_id == CMD_MGET) {
                        parse_off -= consumed; /* hand off to async scatter-gather */
                        mget_fallback = true;
                        break;
                    }

                    cmd_slot_t *s = &slots[num_slots++];
                    memset(s, 0, sizeof(*s));

                    if (cmd.command_id == CMD_PING) {
                        s->resp_len = snprintf(s->resp, sizeof(s->resp), "+PONG\r\n");
                        continue;
                    }
                    if (cmd.command_id != CMD_GET && cmd.command_id != CMD_SET) {
                        s->resp_len = snprintf(s->resp, sizeof(s->resp),
                                               "-ERR Unknown Command Received\r\n");
                        continue;
                    }

                    s->is_set       = (cmd.command_id == CMD_SET);
                    char  *key      = cmd.args[1].ptr;
                    size_t klen     = cmd.args[1].len;
                    unsigned long h = 5381;
                    for (size_t ki = 0; ki < klen; ki++)
                        h = ((h << 5) + h) + (unsigned char)key[ki];
                    int target_shard = (int)(h & (unsigned long)(NUM_CORES - 1));

                    if (target_shard == core_id) {
                        /* LOCAL FAST-PATH — zero latency */
                        if (s->is_set) {
                            int res    = db_set(shard_storage[core_id], key, klen,
                                                cmd.args[2].ptr, cmd.args[2].len);
                            s->success = (res >= 0);
                        } else {
                            db_entry_t *entry = db_get(shard_storage[core_id], key, klen);
                            s->found   = (entry != NULL);
                            if (entry) { s->val_ptr = entry->value; s->val_len = entry->val_len; }
                        }
                    } else {
                        /* REMOTE PATH — dispatch now, do NOT wait */
                        int req_id = highway_alloc_request_id(core_id);
                        if (req_id < 0) {
                            s->resp_len = snprintf(s->resp, sizeof(s->resp),
                                                   "-ERR Internal Pipeline Saturated\r\n");
                            num_slots--; /* no slot consumed for this error */
                            /* still need to emit error: re-add as inline */
                            cmd_slot_t *err = &slots[num_slots++];
                            memset(err, 0, sizeof(*err));
                            err->resp_len = snprintf(err->resp, sizeof(err->resp),
                                                     "-ERR Internal Pipeline Saturated\r\n");
                            continue;
                        }

                        highway_request_t req = {0};
                        req.op_type       = s->is_set ? OP_HIGHWAY_SET : OP_HIGHWAY_GET;
                        req.request_id    = (uint8_t)req_id;
                        req.source_core   = (uint8_t)core_id;
                        req.result_status = HS_PENDING;
                        req.key_ptr       = key;
                        req.key_len       = (uint16_t)klen;
                        if (s->is_set) {
                            req.val_ptr = cmd.args[2].ptr;
                            req.val_len = (uint16_t)cmd.args[2].len;
                        }
                        req.timestamp = (uint32_t)time(NULL);

                        if (!highway_send_request(core_id, target_shard, &req)) {
                            s->resp_len = snprintf(s->resp, sizeof(s->resp),
                                                   "-ERR Highway Saturated\r\n");
                            continue;
                        }

                        /* Request is in flight — record slot, advance to next command */
                        s->is_highway = true;
                        s->req_id     = (uint8_t)req_id;
                        num_highway++;
                    }
                } /* end parse loop (Phase 1) */

                /* ══ Pipeline Phase 2: Parallel collect all highway responses ═══════════
                 * All remote requests are now in flight simultaneously.
                 * Remote cores process them in parallel via their own event
                 * loops.  We poll until every response has landed.
                 *
                 * Total wait ≈ max(individual RTTs) ≈ 1 L3 hop (~100 ns)
                 * instead of num_remote × 100 ns with sequential spin-wait.
                 * ════════════════════════════════════════════════════════════════════════ */
                while (num_highway > 0) {
                    highway_process_requests(core_id); /* drain inbound response lane */
                    for (int si = 0; si < num_slots && num_highway > 0; si++) {
                        cmd_slot_t *s = &slots[si];
                        if (!s->is_highway) continue;
                        if (!highway_request_ready(core_id, s->req_id)) continue;

                        highway_response_t *resp = highway_get_response(core_id, s->req_id);
                        if (s->is_set) {
                            s->success = (resp->result_status == HS_SUCCESS);
                        } else {
                            s->found   = (resp->result_status == HS_FOUND);
                            s->val_ptr = resp->result_ptr;
                            s->val_len = (size_t)resp->result_len;
                        }
                        s->is_highway = false; /* collected */
                        num_highway--;
                    }
                } /* end Phase 2 */

                /* ══ Pipeline Phase 3: Assemble combined response in command order ══
                 * All results (local + highway) are now available.  Write RESP
                 * replies into resp_acc in the original command order
                 * (TCP ordering guarantee).  ONE io_uring send for the entire
                 * pipeline burst — not one per command.
                 * ════════════════════════════════════════════════════════════════════ */
                char resp_acc[READ_BUF_SIZE];
                int  resp_off = 0;

                for (int si = 0; si < num_slots; si++) {
                    cmd_slot_t *s = &slots[si];
                    if (s->resp_len > 0) {
                        /* PING / error / saturation: use pre-built inline string */
                        int safe = (s->resp_len < (int)(sizeof(resp_acc) - resp_off))
                                   ? s->resp_len : (int)(sizeof(resp_acc) - resp_off) - 1;
                        memcpy(resp_acc + resp_off, s->resp, safe);
                        resp_off += safe;
                    } else if (s->is_set) {
                        resp_off += snprintf(resp_acc + resp_off,
                                             sizeof(resp_acc) - resp_off,
                                             s->success ? "+OK\r\n"
                                                        : "-ERR Storage Arena Saturated\r\n");
                    } else { /* GET */
                        if (s->found)
                            resp_off += snprintf(resp_acc + resp_off,
                                                 sizeof(resp_acc) - resp_off,
                                                 "$%zu\r\n%s\r\n",
                                                 s->val_len, s->val_ptr);
                        else
                            resp_off += snprintf(resp_acc + resp_off,
                                                 sizeof(resp_acc) - resp_off, "$-1\r\n");
                    }
                } /* end Phase 3 */

                if (mget_fallback) {
                    /* Delegate MGET to the existing async scatter-gather path */
                    resp_command_t mget_cmd;
                    if (resp_parse_buffer(io_data->buffer + parse_off,
                                          bytes_recv - parse_off, &mget_cmd) >= 0) {
                        if (resp_off > 0) {
                            /* Flush already-accumulated GET/SET responses first */
                            io_context_t *flush_ctx = malloc(sizeof(io_context_t));
                            if (flush_ctx) {
                                flush_ctx->fd      = io_data->fd;
                                flush_ctx->type    = OP_WRITE;
                                flush_ctx->core_id = core_id;
                                memcpy(flush_ctx->buffer, resp_acc, resp_off);
                                struct io_uring_sqe *fsqe = io_uring_get_sqe(&ring);
                                if (fsqe) {
                                    io_uring_prep_send(fsqe, flush_ctx->fd,
                                                       flush_ctx->buffer, resp_off, 0);
                                    io_uring_sqe_set_data(fsqe, flush_ctx);
                                    io_uring_submit(&ring);
                                } else {
                                    free(flush_ctx);
                                }
                                resp_off = 0;
                            }
                        }
                        handle_async_mget(core_id, io_data, &mget_cmd, pending_io,
                                          &pending_io_count);
                        if (io_data->type == OP_WRITE) {
                            struct io_uring_sqe *wsqe = io_uring_get_sqe(&ring);
                            if (wsqe) {
                                io_uring_prep_send(wsqe, io_data->fd,
                                                   io_data->buffer,
                                                   strlen(io_data->buffer), 0);
                                io_uring_sqe_set_data(wsqe, io_data);
                                io_uring_submit(&ring);
                            }
                        }
                    }
                    break;
                }

                if (resp_off > 0) {
                    memcpy(io_data->buffer, resp_acc, resp_off);
                    io_data->type = OP_WRITE;
                    struct io_uring_sqe *write_sqe = io_uring_get_sqe(&ring);
                    int send_flags = 0;
                    #ifdef MSG_ZEROCOPY
                    send_flags = MSG_ZEROCOPY;
                    #endif
                    io_uring_prep_send(write_sqe, io_data->fd, io_data->buffer,
                                       resp_off, send_flags);
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

            case OP_HIGHWAY_WAIT_STATE:
                // Response arrives via the highway polling loop above; no io_uring action needed.
                break;
        }

        io_uring_cqe_seen(&ring, cqe);
    }
    return NULL;
}
