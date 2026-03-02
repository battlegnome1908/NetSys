/*
 * udp_client.c - UDP file-transfer client
 * Usage: ./udp_client <hostname> <port>
 */

#include "packets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/select.h>

/* Forward declarations */
void print_command_prompt(void);
void parse_command_input(char *input_line, char *command, char *arg);
int  send_reliable(int sockfd, struct sockaddr_in *addr, Packet *packet);
void cmd_ls(int sockfd, struct sockaddr_in *addr);
void cmd_get(int sockfd, struct sockaddr_in *addr, const char *filename);
void cmd_put(int sockfd, struct sockaddr_in *addr, const char *filename);
void cmd_delete(int sockfd, struct sockaddr_in *addr, const char *filename);
void cmd_exit(int sockfd, struct sockaddr_in *addr);
int  send_file(int sockfd, struct sockaddr_in *addr, const char *filepath);
int  receive_file(int sockfd, struct sockaddr_in *addr, const char *filepath);

/* Wrapper for fatal errors */
void error(const char *msg) {
    perror(msg);
    exit(0);
}

int main(int argc, char **argv) {
    int                sockfd;
    int                port_num;
    struct sockaddr_in server_addr;
    struct hostent    *server_host;
    char              *server_hostname;
    char               input_line[BUFSIZE];
    char               command[BUFSIZE];
    char               cmd_arg[BUFSIZE];

    if (argc != 3) {
        fprintf(stderr, "usage: %s <hostname> <port>\n", argv[0]);
        exit(0);
    }
    server_hostname = argv[1];
    port_num        = atoi(argv[2]);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");

    server_host = gethostbyname(server_hostname);
    if (server_host == NULL) {
        fprintf(stderr, "ERROR: no such host '%s'\n", server_hostname);
        exit(0);
    }

    bzero((char *)&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    bcopy((char *)server_host->h_addr,
          (char *)&server_addr.sin_addr.s_addr, server_host->h_length);
    server_addr.sin_port = htons(port_num);

    /* REPL: read commands from stdin and dispatch to the server */
    while (1) {
        print_command_prompt();
        fflush(stdout);
        if (fgets(input_line, BUFSIZE, stdin) == NULL)
            break;
        parse_command_input(input_line, command, cmd_arg);
        if (command[0] == '\0')
            continue;

        if (strcmp(command, "ls") == 0) {
            cmd_ls(sockfd, &server_addr);
        } else if (strcmp(command, "get") == 0) {
            if (cmd_arg[0] == '\0') { fprintf(stderr, "usage: get <filename>\n"); continue; }
            cmd_get(sockfd, &server_addr, cmd_arg);
        } else if (strcmp(command, "put") == 0) {
            if (cmd_arg[0] == '\0') { fprintf(stderr, "usage: put <filename>\n"); continue; }
            cmd_put(sockfd, &server_addr, cmd_arg);
        } else if (strcmp(command, "delete") == 0) {
            if (cmd_arg[0] == '\0') { fprintf(stderr, "usage: delete <filename>\n"); continue; }
            cmd_delete(sockfd, &server_addr, cmd_arg);
        } else if (strcmp(command, "exit") == 0) {
            cmd_exit(sockfd, &server_addr);
            break;
        } else {
            fprintf(stderr, "Unknown command: %s\n", command);
        }
    }

    close(sockfd);
    return 0;
}

/* Print the interactive command prompt */
void print_command_prompt(void) {
    printf("Enter command (ls, get <file>, put <file>, delete <file>, exit): ");
}

/* Parse a raw input line into a command string and an optional argument */
void parse_command_input(char *input_line, char *command, char *arg) {
    char *token = strtok(input_line, " \n");
    if (token != NULL) {
        strcpy(command, token);
        token = strtok(NULL, " \n");
        if (token != NULL)
            strcpy(arg, token);
        else
            arg[0] = '\0';
    } else {
        command[0] = '\0';
        arg[0]     = '\0';
    }
}

/*
 * send_reliable - transmit a packet and block until a matching ACK is
 * received, retrying up to MAX_RETRIES times before giving up.
 * Returns 0 on success, -1 on failure.
 */
int send_reliable(int sockfd, struct sockaddr_in *addr, Packet *packet) {
    int retry_count = 0;
    while (retry_count < MAX_RETRIES) {
        if (send_packet(sockfd, packet, addr) < 0)
            return -1;

        Packet ack_packet;
        int bytes_received = receive_packet_timeout(sockfd, &ack_packet,
                                                    addr, TIMEOUT_MS);
        if (bytes_received < 0) {
            return -1;
        } else if (bytes_received == 0) {
            /* Timeout — retransmit */
            retry_count++;
            continue;
        }

        if (ack_packet.type == TYPE_ACK &&
            ack_packet.ack_num == packet->seq_num + packet->data_len)
            return 0; /* success */

        /* Wrong ACK — retransmit */
        retry_count++;
    }
    return -1; /* exceeded MAX_RETRIES */
}

/* Send TYPE_LS to the server and print the returned file listing */
void cmd_ls(int sockfd, struct sockaddr_in *addr) {
    Packet cmd_packet;
    make_packet(&cmd_packet, TYPE_LS, 0, NULL, 0);
    if (send_reliable(sockfd, addr, &cmd_packet) < 0) {
        fprintf(stderr, "ERROR: failed to send ls command\n");
        return;
    }

    Packet response_packet;
    int bytes_received = receive_packet_timeout(sockfd, &response_packet,
                                                addr, TIMEOUT_MS);
    if (bytes_received < 0) {
        fprintf(stderr, "ERROR: failed to receive ls response\n");
    } else if (bytes_received == 0) {
        fprintf(stderr, "ERROR: timeout waiting for ls response\n");
    } else if (response_packet.type == TYPE_LS_RESPONSE) {
        printf("Files on server:\n%s\n", response_packet.data);
    } else {
        fprintf(stderr, "ERROR: unexpected packet type in ls response\n");
    }
}

/* Request a file from the server and save it in the current directory */
void cmd_get(int sockfd, struct sockaddr_in *addr, const char *filename) {
    Packet cmd_packet;
    make_packet(&cmd_packet, TYPE_GET, 0, filename, (uint32_t)strlen(filename));
    if (send_reliable(sockfd, addr, &cmd_packet) < 0) {
        fprintf(stderr, "ERROR: failed to send get command\n");
        return;
    }
    if (receive_file(sockfd, addr, filename) < 0)
        fprintf(stderr, "ERROR: failed to receive file '%s'\n", filename);
}

/* Upload a local file to the server */
void cmd_put(int sockfd, struct sockaddr_in *addr, const char *filename) {
    Packet cmd_packet;
    make_packet(&cmd_packet, TYPE_PUT, 0, filename, (uint32_t)strlen(filename));
    if (send_reliable(sockfd, addr, &cmd_packet) < 0) {
        fprintf(stderr, "ERROR: failed to send put command\n");
        return;
    }
    if (send_file(sockfd, addr, filename) < 0)
        fprintf(stderr, "ERROR: failed to send file '%s'\n", filename);
}

/* Ask the server to delete a file and print the server's response */
void cmd_delete(int sockfd, struct sockaddr_in *addr, const char *filename) {
    Packet cmd_packet;
    make_packet(&cmd_packet, TYPE_DELETE, 0, filename, (uint32_t)strlen(filename));
    if (send_reliable(sockfd, addr, &cmd_packet) < 0) {
        fprintf(stderr, "ERROR: failed to send delete command\n");
        return;
    }

    Packet response_packet;
    int bytes_received = receive_packet_timeout(sockfd, &response_packet,
                                                addr, TIMEOUT_MS);
    if (bytes_received < 0) {
        fprintf(stderr, "ERROR: failed to receive delete response\n");
    } else if (bytes_received == 0) {
        fprintf(stderr, "ERROR: timeout waiting for delete response\n");
    } else if (response_packet.type == TYPE_DELETE_RESPONSE) {
        printf("Delete: %s\n", response_packet.data);
    } else {
        fprintf(stderr, "ERROR: unexpected packet type in delete response\n");
    }
}

/* Send TYPE_EXIT; the server ACKs and terminates */
void cmd_exit(int sockfd, struct sockaddr_in *addr) {
    Packet cmd_packet;
    make_packet(&cmd_packet, TYPE_EXIT, 0, NULL, 0);
    if (send_reliable(sockfd, addr, &cmd_packet) < 0)
        fprintf(stderr, "ERROR: failed to send exit command\n");
    else
        printf("Exiting client...\n");
}

/*
 * send_file - open the file at filepath and transmit it in BUFSIZE chunks
 * using stop-and-wait reliability, followed by a TYPE_FILE_END marker.
 */
int send_file(int sockfd, struct sockaddr_in *addr, const char *filepath) {
    FILE *file = fopen(filepath, "rb");
    if (!file) {
        perror("ERROR opening file");
        return -1;
    }

    uint32_t seq_num = 0;
    char     read_buffer[BUFSIZE];
    size_t   chunk_size;

    while ((chunk_size = fread(read_buffer, 1, BUFSIZE, file)) > 0) {
        Packet data_packet;
        make_packet(&data_packet, TYPE_FILE_DATA, seq_num,
                    read_buffer, (uint32_t)chunk_size);
        if (send_reliable(sockfd, addr, &data_packet) < 0) {
            fprintf(stderr, "ERROR: failed to send file chunk\n");
            fclose(file);
            return -1;
        }
        seq_num += (uint32_t)chunk_size;
    }

    /* Signal end of file */
    Packet eof_packet;
    make_packet(&eof_packet, TYPE_FILE_END, seq_num, NULL, 0);
    if (send_reliable(sockfd, addr, &eof_packet) < 0) {
        fprintf(stderr, "ERROR: failed to send EOF packet\n");
        fclose(file);
        return -1;
    }

    fclose(file);
    return 0;
}

/*
 * receive_file - receive file chunks from the server and write them to filepath.
 * ACKs every TYPE_FILE_DATA chunk and the final TYPE_FILE_END marker so the
 * sender's send_reliable unblocks after each packet.
 */
int receive_file(int sockfd, struct sockaddr_in *addr, const char *filepath) {
    FILE *file = fopen(filepath, "wb");
    if (!file) {
        perror("ERROR opening file");
        return -1;
    }

    uint32_t expected_seq_num = 0;
    while (1) {
        Packet recv_packet;
        int bytes_received = receive_packet_timeout(sockfd, &recv_packet,
                                                    addr, TIMEOUT_MS);
        if (bytes_received < 0) {
            fprintf(stderr, "ERROR: failed to receive file data\n");
            fclose(file);
            return -1;
        } else if (bytes_received == 0) {
            fprintf(stderr, "ERROR: timeout waiting for file data\n");
            fclose(file);
            return -1;
        }

        if (recv_packet.type == TYPE_FILE_DATA) {
            /* Only write in-order data; discard retransmitted duplicates */
            if (recv_packet.seq_num == expected_seq_num) {
                fwrite(recv_packet.data, 1, recv_packet.data_len, file);
                expected_seq_num += recv_packet.data_len;
            }
            /* Cumulative ACK: tells the sender the next byte we expect */
            Packet ack_packet;
            make_packet(&ack_packet, TYPE_ACK, 0, NULL, 0);
            ack_packet.ack_num = expected_seq_num;
            send_packet(sockfd, &ack_packet, addr);

        } else if (recv_packet.type == TYPE_FILE_END) {
            /* ACK the EOF marker so sender's send_reliable returns */
            Packet ack_packet;
            make_packet(&ack_packet, TYPE_ACK, 0, NULL, 0);
            ack_packet.ack_num = recv_packet.seq_num + recv_packet.data_len;
            send_packet(sockfd, &ack_packet, addr);
            break;

        } else {
            fprintf(stderr, "ERROR: unexpected packet type %d\n",
                    recv_packet.type);
            fclose(file);
            return -1;
        }
    }

    fclose(file);
    return 0;
}
