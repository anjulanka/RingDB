/*
 * Copyright 2026 The RingDB Authors
 * Licensed under the AGPLv3 / SSPLv1 / RSALv2 Tri-License Framework
 */

#define _GNU_SOURCE // Required for CPU affinity macros in Linux
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "ring_db.h"
#include "ring_highway.h"

#define PORT 6379
/* NUM_CORES is defined in ring_db.h — do not redefine here. */

// Forward declaration of our async network runner located in iouring_backend.c
void* iouring_worker_loop(void* arg);

// Thread argument structure to pass context cleanly down to each core shard
typedef struct {
    int core_id;
    int server_fd;
} worker_ctx_t;

int main() {
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;

    printf("=========================================================\n");
    printf("🌀 RingDB Key-Value Engine - Initializing Bootstrap Layer\n");
    printf("=========================================================\n");

    // 1. Create the master internet TCP socket stream
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("[-] Critical Error: Master socket creation failed");
        exit(EXIT_FAILURE);
    }

    // 2. Set socket options to allow port reuse and balance client streams natively
    // SO_REUSEPORT lets the kernel distribute requests straight across our worker rings [🔑]
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        perror("[-] Critical Error: setsockopt configuration failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // 3. Define the server binding address (Listen on all network interfaces)
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // 4. Bind the socket file descriptor to Port 6379
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("[-] Critical Error: Network binding to Port 6379 failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // 5. Open the gate for connections with a massive kernel backlog queue
    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("[-] Critical Error: Socket listen state failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("[+] Network master gate successfully opened on Port %d\n", PORT);
    printf("[+] Spin up thread pool: %d Shared-Nothing Workers configured.\n", NUM_CORES);

    // ✅ FIX 1: Initialize Lockless Highways for inter-core communication
    highway_init_matrix(NUM_CORES);
    
    // ✅ Phase 3: Initialize Async Request/Response Trackers
    highway_init_request_trackers(NUM_CORES);

    pthread_t threads[NUM_CORES];
    worker_ctx_t worker_contexts[NUM_CORES];

    // 6. Spawn the isolated multi-threaded core matrix
    for (int i = 0; i < NUM_CORES; i++) {
        worker_contexts[i].core_id = i;
        worker_contexts[i].server_fd = server_fd;

        // Launch the async loop thread
        if (pthread_create(&threads[i], NULL, iouring_worker_loop, &worker_contexts[i]) != 0) {
            perror("[-] Critical Error: Failed to spawn thread worker");
            close(server_fd);
            exit(EXIT_FAILURE);
        }

        // --- 🔥 HARDWARE CPU AFFINITY PINNING ---
        // Force the OS to lock Thread 'i' directly onto physical CPU Core 'i'
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(i, &cpuset);

        if (pthread_setaffinity_np(threads[i], sizeof(cpu_set_t), &cpuset) != 0) {
            fprintf(stderr, "[-] Warning: Failed to lock thread %d onto CPU Core %d\n", i, i);
        } else {
            printf("[+] Worker Shard [%d] permanently pinned to CPU Hardware Core #%d\n", i, i);
        }
    }

    printf("=========================================================\n");
    printf("[*] RingDB engine is fully live and running. Press Ctrl+C to halt.\n");
    printf("=========================================================\n");

    // 7. Keep the master supervisor thread alive by waiting for workers
    for (int i = 0; i < NUM_CORES; i++) {
        pthread_join(threads[i], NULL);
    }

    for (int i = 0; i < NUM_CORES; i++) {
        if (shard_storage[i]) {
            arena_destroy(shard_storage[i]->arena); // Safely frees the 1GB RAM blocks
            free(shard_storage[i]);
        }
    }

    // Clean shutdown closure
    close(server_fd);
    return 0;
}
