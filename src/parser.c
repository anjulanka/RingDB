/*
 * Copyright 2026 The RingDB Authors
 * Licensed under the AGPLv3 / SSPLv1 / RSALv2 Tri-License Framework
 */

#include "parser.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// Quick helper to safely parse integer lengths out of the raw text buffer
static char* parse_resp_length(char *ptr, char *end, int *out_len) {
    int val = 0;
    while (ptr < end && *ptr != '\r') {
        if (*ptr >= '0' && *ptr <= '9') {
            val = (val * 10) + (*ptr - '0');
        }
        ptr++;
    }
    if (ptr + 1 >= end || *ptr != '\r' || *(ptr + 1) != '\n') return NULL;
    *out_len = val;
    return ptr + 2; // Move read head cleanly past "\r\n"
}

int resp_parse_buffer(char *buffer, size_t buffer_len, resp_command_t *out_command) {
    char *ptr = buffer;
    char *end = buffer + buffer_len;
    
    // Reset out_command fields
    out_command->command_id = 0;
    out_command->arg_count = 0;

    // Validate minimum protocol frame length and structural array marker '*'
    if (buffer_len < 4 || *ptr != '*') return -1;
    ptr++;

    // Extract total argument count array length
    int total_args = 0;
    ptr = parse_resp_length(ptr, end, &total_args);
    if (!ptr || total_args <= 0 || total_args > MAX_RESP_ARGS) return -1;

    // Extract each individual bulk string argument token lineally
    for (int i = 0; i < total_args; i++) {
        if (ptr >= end || *ptr != '$') return -1;
        ptr++;

        int arg_len = 0;
        ptr = parse_resp_length(ptr, end, &arg_len);
        if (!ptr || ptr + arg_len + 2 > end) return -1;

        // Perform zero-copy allocation: store direct memory reference and length
        out_command->args[i].ptr = ptr;
        out_command->args[i].len = arg_len;
        out_command->arg_count++;

        // Advance read head past the token payload and its trailing "\r\n" boundary marker
        ptr += arg_len + 2;
    }

    // --- OPTIMIZATION: INLINE COMMAND HASHING ---
    // Extract the primary command verb token string (always the first item in RESP arrays)
    resp_arg_t *cmd_token = &out_command->args[0];
    uint32_t hash = 0;

    // Convert up to 4 characters of the command verb string directly into a single 32-bit integer
    for (size_t j = 0; j < cmd_token->len && j < 4; j++) {
        hash |= ((uint32_t)toupper((unsigned char)cmd_token->ptr[j])) << (j * 8);
    }
    
    out_command->command_id = hash;
    return 0; // Parsing execution completed successfully
}
