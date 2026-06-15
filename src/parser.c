/*
 * Copyright 2026 The RingDB Authors
 * Licensed under the AGPLv3 / SSPLv1 / RSALv2 Tri-License Framework
 */

#include "parser.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// Quick helper to safely parse integer lengths out of the raw text buffer
// ✅ FIX 3: Use SIMD-friendly memchr instead of byte-by-byte loop
static char* parse_resp_length(char *ptr, char *end, int *out_len) {
    // Use memchr for vectorized search (16-32 bytes at a time)
    char *newline = memchr(ptr, '\n', end - ptr);
    if (!newline || newline == ptr || *(newline - 1) != '\r') {
        return NULL;
    }
    
    // Parse the number in range [ptr, newline-1)
    int val = 0;
    int is_negative = 0;
    
    while (ptr < newline - 1) {
        if (*ptr == '-') {
            is_negative = 1;
        } else if (*ptr >= '0' && *ptr <= '9') {
            val = (val * 10) + (*ptr - '0');
        } else {
            return NULL;  // Invalid character in length field
        }
        ptr++;
    }
    
    *out_len = is_negative ? -1 : val;
    return newline + 1;
}

int resp_parse_buffer(char *buffer, size_t buffer_len, resp_command_t *out_command) {
    char *ptr = buffer;
    char *end = buffer + buffer_len;
    
    out_command->command_id = 0;
    out_command->arg_count = 0;

    if (buffer_len < 4 || *ptr != '*') return -1;
    ptr++;

    int total_args = 0;
    ptr = parse_resp_length(ptr, end, &total_args);
    if (!ptr || total_args <= 0 || total_args > MAX_RESP_ARGS) return -1;

    for (int i = 0; i < total_args; i++) {
        if (ptr >= end || *ptr != '$') return -1;
        ptr++;

        int arg_len = 0;
        ptr = parse_resp_length(ptr, end, &arg_len);
        if (!ptr || ptr + arg_len + 2 > end) return -1;

        // Perform zero-copy tracking references
        out_command->args[i].ptr = ptr;
        out_command->args[i].len = arg_len;
        out_command->arg_count++;

        // This isolates the string segment safely inside the network buffer memory line.
        ptr[arg_len] = '\0';

        // Advance read head past the token payload and its trailing "\r\n" markers
        ptr += arg_len + 2;
    }

    // --- OPTIMIZATION: INLINE COMMAND HASHING ---
    resp_arg_t *cmd_token = &out_command->args[0]; // Index 0 is always the command verb (SET/GET)
    uint32_t hash = 0;

    for (size_t j = 0; j < cmd_token->len && j < 4; j++) {
        hash |= ((uint32_t)toupper((unsigned char)cmd_token->ptr[j])) << (j * 8);
    }
    
    out_command->command_id = hash;
    return 0; 
}
