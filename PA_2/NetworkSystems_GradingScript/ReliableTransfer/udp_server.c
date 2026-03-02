/*
 * udp_server.c - UDP file-transfer server
 * Usage: ./udp_server <port>
 */

#include "packets.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>

/* Forward declarations */
int  send_reliable(int sockfd, struct sockaddr_in *addr, Packet *packet);
void dispatch_command(int sockfd, struct sockaddr_in *client_addr, Packet *packet);
void handle_ls_command(int sockfd, struct sockaddr_in *client_addr);
void handle_get_command(int sockfd, struct sockaddr_in *client_addr, const char *filename);
void handle_put_command(int sockfd, struct sockaddr_in *client_addr, const char *filename);
void handle_delete_command(int sockfd, struct sockaddr_in *client_addr, const char *filename);
void handle_exit_command(int sockfd, struct sockaddr_in *client_addr);
int  send_file(int sockfd, struct sockaddr_in *addr, const char *filepath);
int  receive_file(int sockfd, struct sockaddr_in *addr, const char *filepath);

/* Wrapper for fatal errors */
void error(const char *msg) {
    perror(msg);
    exit(1);
}

int main(int argc, char **argv) {
    int               sockfd;
    int               port_num;
    socklen_t         client_addr_len;
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    int               reuse_opt;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }
    port_num = atoi(argv[1]);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");

    /* Allow immediate port reuse after server restart */
    reuse_opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
               (const void *)&reuse_opt, sizeof(int));

    bzero((char *)&server_addr, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port        = htons((unsigned short)port_num);

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
        error("ERROR on binding");

    /* Main receive-dispatch loop */
    Packet recv_packet;
    client_addr_len = sizeof(client_addr);
    while (1) {
        if (recvfrom(sockfd, &recv_packet, sizeof(Packet), 0,
                     (struct sockaddr *)&client_addr, &client_addr_len) < 0)
            error("ERROR in recvfrom");
        dispatch_command(sockfd, &client_addr, &recv_packet);
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

/*
 * dispatch_command - ACK the incoming command packet, then call the
 * appropriate handler based on the packet type.
 */
void dispatch_command(int sockfd, struct sockaddr_in *client_addr, Packet *packet) {
    /* ACK the command so the client's send_reliable unblocks */
    Packet ack_packet;
    make_packet(&ack_packet, TYPE_ACK, 0, NULL, 0);
    ack_packet.ack_num = packet->seq_num + packet->data_len;
    send_packet(sockfd, &ack_packet, client_addr);

    switch (packet->type) {
        case TYPE_LS:
            handle_ls_command(sockfd, client_addr);
            break;
        case TYPE_GET:
            packet->data[packet->data_len] = '\0';
            handle_get_command(sockfd, client_addr, packet->data);
            break;
        case TYPE_PUT:
            packet->data[packet->data_len] = '\0';
            handle_put_command(sockfd, client_addr, packet->data);
            break;
        case TYPE_DELETE:
            packet->data[packet->data_len] = '\0';
            handle_delete_command(sockfd, client_addr, packet->data);
            break;
        case TYPE_EXIT:
            handle_exit_command(sockfd, client_addr);
            break;
        default:
            fprintf(stderr, "WARNING: unknown packet type %d — ignoring\n",
                    packet->type);
            break;
    }
}

/* List all regular files in the server's working directory and send the result */
void handle_ls_command(int sockfd, struct sockaddr_in *client_addr) {
    DIR *dir_stream = opendir(".");
    if (!dir_stream) {
        perror("ERROR opening directory");
        return;
    }

    char file_list[BUFSIZE] = {0};
    struct dirent *dir_entry;
    while ((dir_entry = readdir(dir_stream)) != NULL) {
        if (dir_entry->d_type == DT_REG) {
            strncat(file_list, dir_entry->d_name,
                    sizeof(file_list) - strlen(file_list) - 1);
            strncat(file_list, "\n",
                    sizeof(file_list) - strlen(file_list) - 1);
        }
    }
    closedir(dir_stream);

    /* Client reads this directly after the command ACK; no reliability needed */
    Packet response_packet;
    make_packet(&response_packet, TYPE_LS_RESPONSE, 0,
                file_list, (uint32_t)strlen(file_list));
    send_packet(sockfd, &response_packet, client_addr);
}

/* Transmit the requested file to the client */
void handle_get_command(int sockfd, struct sockaddr_in *client_addr,
                        const char *filename) {
    if (send_file(sockfd, client_addr, filename) < 0)
        fprintf(stderr, "ERROR: failed to send file '%s'\n", filename);
}

/* Receive and store a file uploaded by the client */
void handle_put_command(int sockfd, struct sockaddr_in *client_addr,
                        const char *filename) {
    if (receive_file(sockfd, client_addr, filename) < 0)
        fprintf(stderr, "ERROR: failed to receive file '%s'\n", filename);
}

/* Delete the named file and inform the client of the result */
void handle_delete_command(int sockfd, struct sockaddr_in *client_addr,
                           const char *filename) {
    Packet response_packet;
    if (remove(filename) == 0)
        make_packet(&response_packet, TYPE_DELETE_RESPONSE, 0, "OK", 2);
    else
        make_packet(&response_packet, TYPE_DELETE_RESPONSE, 0, "ERROR", 5);
    send_packet(sockfd, &response_packet, client_addr);
}

/* Gracefully shut down the server process */
void handle_exit_command(int sockfd, struct sockaddr_in *client_addr) {
    (void)sockfd; (void)client_addr;
    exit(0);
}

/*
 * send_file - Go-Back-N sliding window sender.
 *
 * Keeps up to WINDOW_SIZE packets in flight simultaneously.  Each outer-loop
 * iteration fills any empty window slots, transmits newly-added packets, then
 * waits for one ACK.
 *
 *  - Valid cumulative ACK  → slide window forward, fill + send new packets,
 *                            reset retry counter.
 *  - Timeout               → retransmit the entire current window (Go Back N),
 *                            increment retry counter.
 *
 * receive_file() is unchanged: it already sends cumulative ACKs and discards
 * out-of-order packets, which is the correct GBN receiver behaviour.
 */
int send_file(int sockfd, struct sockaddr_in *addr, const char *filepath) {
    FILE *file = fopen(filepath, "rb");
    if (!file) { perror("ERROR opening file"); return -1; }

    Packet   window[WINDOW_SIZE]; /* circular buffer of unACKed packets      */
    int      win_count  = 0;     /* packets currently in the window          */
    int      win_sent   = 0;     /* index of first unsent packet in window[] */
    uint32_t next_seq   = 0;     /* byte offset for the next new packet      */
    int      file_done  = 0;     /* all file bytes have been read            */
    int      eof_queued = 0;     /* TYPE_FILE_END packet is in the window    */
    int      transfer_done = 0;
    int      retries    = 0;

    while (!transfer_done && retries < MAX_RETRIES) {

        /* ── 1. Fill empty window slots ──────────────────────────────── */
        while (win_count < WINDOW_SIZE && !eof_queued) {
            char   buf[DATA_SIZE];
            size_t chunk = file_done ? 0 : fread(buf, 1, DATA_SIZE, file);
            if (chunk > 0) {
                make_packet(&window[win_count], TYPE_FILE_DATA,
                            next_seq, buf, (uint32_t)chunk);
                next_seq += (uint32_t)chunk;
            } else {
                /* No more data — queue the EOF marker */
                file_done = 1;
                make_packet(&window[win_count], TYPE_FILE_END, next_seq, NULL, 0);
                eof_queued = 1;
            }
            win_count++;
        }

        if (win_count == 0) break; /* nothing to do */

        /* ── 2. Transmit any newly-added packets ─────────────────────── */
        for (int i = win_sent; i < win_count; i++)
            send_packet(sockfd, &window[i], addr);
        win_sent = win_count;

        /* ── 3. Wait for one ACK (up to TIMEOUT_MS) ──────────────────── */
        Packet ack_pkt;
        int received = receive_packet_timeout(sockfd, &ack_pkt, addr, TIMEOUT_MS);
        if (received < 0) { fclose(file); return -1; }

        if (received == 0) {
            /* Timeout: Go Back N — mark entire window as unsent so step 2
             * retransmits all of them on the next iteration.             */
            retries++;
            win_sent = 0;
            continue;
        }

        if (ack_pkt.type != TYPE_ACK) continue; /* ignore non-ACK traffic */

        /* ── 4. Cumulative ACK: slide window forward ──────────────────── */
        int acked = 0;
        for (int i = 0; i < win_count; i++) {
            if (ack_pkt.ack_num == window[i].seq_num + window[i].data_len) {
                acked = i + 1;
                if (window[i].type == TYPE_FILE_END) transfer_done = 1;
                break;
            }
        }

        if (acked > 0) {
            memmove(&window[0], &window[acked],
                    (win_count - acked) * sizeof(Packet));
            win_count -= acked;
            win_sent  -= acked;
            if (win_sent < 0) win_sent = 0;
            retries = 0;
        }
        /* acked == 0 means a duplicate/stale ACK; loop and wait for the next */
    }

    fclose(file);
    return transfer_done ? 0 : -1;
}

/*
 * receive_file - receive file chunks from the sender and write them to filepath.
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
            /* Cumulative ACK: tells sender the next byte we expect */
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
