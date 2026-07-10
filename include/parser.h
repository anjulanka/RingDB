/*
 * Copyright 2026 The RingDB Authors
 * Licensed under the AGPLv3 / SSPLv1 / RSALv2 Tri-License Framework
 */

#ifndef PARSER_H
#define PARSER_H

#include <stddef.h>
#include <stdint.h>

/* MAX_RESP_ARGS: maximum arguments accepted per RESP command.
 * SET/GET use 3/2 args. MGET supports up to MAX_RESP_ARGS-1 keys.
 * 64 lets clients issue MGET with 63 keys in one round trip — well above
 * the typical 10-20 key MGET used in production. */
#define MAX_RESP_ARGS 64

// Fast 32-bit inline integer constants representing parsed Redis commands
#define CMD_PING  0x474e4950  // "PING" packed as an integer
#define CMD_GET   0x00544547  // "GET" packed as an integer
#define CMD_SET   0x00544553  // "SET" packed as an integer
#define CMD_DEL   0x004c4544  // "DEL" packed as an integer
#define CMD_MGET  0x5445474d  // "MGET" packed as an inline 32-bit integer

// Zero-copy argument tracking token
typedef struct {
    char *ptr;   // Direct address point to where the token string starts inside the network buffer
    size_t len;  // Length of the token string segment
} resp_arg_t;

// Context structure holding parsed RESP tokens
typedef struct {
    uint32_t command_id;              // Hashed integer ID of the command (e.g., CMD_GET)
    int arg_count;                    // Number of arguments extracted
    resp_arg_t args[MAX_RESP_ARGS];   // Array of zero-copy references to tokens
} resp_command_t;

// Parsing execution functions
int resp_parse_buffer(char *buffer, size_t buffer_len, resp_command_t *out_command);

#endif // PARSER_H
