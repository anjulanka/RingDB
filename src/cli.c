/*
 * Copyright 2026 The RingDB Authors
 * Licensed under the AGPLv3 / SSPLv1 / RSALv2 Tri-License Framework
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define SERVER_IP "127.0.0.1"
#define PORT 6379
#define BUFFER_SIZE 4096

// Helper to convert plain text words into standard Redis RESP format array blocks
void serialize_to_resp(char *input, char *output, size_t max_len) {
    char *words[16];
    int count = 0;
    
    // Split the typed string line by spaces
    char *token = strtok(input, " \n\r");
    while (token && count < 16) {
        words[count++] = token;
        token = strtok(NULL, " \n\r");
    }
    
    if (count == 0) {
        output[0] = '\0';
        return;
    }
    
    // Build the RESP array header line (e.g., *3\r\n)
    size_t offset = snprintf(output, max_len, "*%d\r\n", count);
    
    // Append each word token as a bulk string entry line
    for (int i = 0; i < count; i++) {
        offset += snprintf(output + offset, max_len - offset, "$%zu\r\n%s\r\n", strlen(words[i]), words[i]);
    }
}

// Helper to parse and pretty-print raw incoming RESP server responses
void print_resp_response(char *response) {
    if (response[0] == '+') {
        // Simple String Response (e.g., +OK) -> Print green text
        printf("\033[0;32m%s\033[0m\n", response + 1);
    } else if (response[0] == '-') {
        // Error String Response (e.g., -ERR) -> Print red text
        printf("\033[0;31m(error) %s\033[0m\n", response + 1);
    } else if (response[0] == '$') {
        // Bulk String Response (e.g., $5\r\nvalue\r\n)
        char *ptr = strchr(response, '\r');
        if (ptr && *(ptr + 2) == '-') {
            printf("(nil)\n"); // Null database record check
        } else if (ptr) {
            char *data_start = strchr(ptr + 2, '\n');
            if (data_start) {
                printf("\"%s\"\n", data_start + 1);
            }
        }
    } else {
        printf("%s\n", response); // Fallback raw string output
    }
}

int main() {
    int sock_fd;
    struct sockaddr_in server_addr;
    char input_buffer[BUFFER_SIZE];
    char resp_buffer[BUFFER_SIZE];
    char recv_buffer[BUFFER_SIZE];

    printf("=========================================================\n");
    printf("🌀 RingDB Interactive Command Line Utility (ringdb-cli)\n");
    printf("Connecting to instance on %s:%d...\n", SERVER_IP, PORT);
    printf("=========================================================\n");

    // 1. Establish standard connection socket line
    if ((sock_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("[-] Error: CLI socket initialization failed");
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    
    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
        perror("[-] Error: Invalid loopback network target address");
        close(sock_fd);
        exit(EXIT_FAILURE);
    }

    if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("[-] Error: Connection to ringdb-server instance failed! Is it running?");
        close(sock_fd);
        exit(EXIT_FAILURE);
    }

    printf("[+] Connected! Type commands (e.g., 'PING', 'SET x y', 'GET x'). Type 'exit' to quit.\n\n");

    // 2. Main Prompt Loop
    while (1) {
        printf("ringdb> ");
        fflush(stdout);

        if (!fgets(input_buffer, sizeof(input_buffer), stdin)) break;

        // Strip trailing newline markers to clean input
        input_buffer[strcspn(input_buffer, "\n\r")] = 0;

        if (strlen(input_buffer) == 0) continue;
        if (strcasecmp(input_buffer, "exit") == 0) break;

        // Clean memory buffers
        memset(resp_buffer, 0, sizeof(resp_buffer));
        memset(recv_buffer, 0, sizeof(recv_buffer));

        // 3. Serialize plain console strings into RESP bytes
        serialize_to_resp(input_buffer, resp_buffer, sizeof(resp_buffer));
        if (strlen(resp_buffer) == 0) continue;

        // 4. Send packet down socket wire line to server
        if (send(sock_fd, resp_buffer, strlen(resp_buffer), 0) < 0) {
            perror("[-] Error: Data transmission transmission link failed");
            break;
        }

        // 5. Read back response string packet
        int bytes_read = recv(sock_fd, recv_buffer, sizeof(recv_buffer) - 1, 0);
        if (bytes_read <= 0) {
            printf("[-] Connection closed by database server instance.\n");
            break;
        }

        recv_buffer[bytes_read] = '\0';

        // 6. Output pretty color formatted response lines
        print_resp_response(recv_buffer);
    }

    close(sock_fd);
    printf("Goodbye!\n");
    return 0;
}
