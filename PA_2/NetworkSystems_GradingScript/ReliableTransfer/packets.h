#ifndef PACKET_HANDLER_H
#define PACKET_HANDLER_H

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>

/* Packet type identifiers */
#define TYPE_CMD             0   /* generic command */
#define TYPE_ACK             1   /* acknowledgement */
#define TYPE_DATA            2   /* data chunk */
#define TYPE_END             3   /* end of transfer */
#define TYPE_NOACK           4   /* error / negative acknowledgement */
#define TYPE_LS              5   /* list-files command */
#define TYPE_LS_RESPONSE     6   /* list-files response */
#define TYPE_FILE_DATA       7   /* file data chunk */
#define TYPE_FILE_END        8   /* end of file transfer */
#define TYPE_GET             9   /* get-file command */
#define TYPE_PUT             10  /* put-file command */
#define TYPE_DELETE          11  /* delete-file command */
#define TYPE_EXIT            12  /* exit command */
#define TYPE_DELETE_RESPONSE 13  /* delete-file response */

/* Protocol tuning constants */
#define MAX_RETRIES  5
#define WINDOW_SIZE  5     /* Go-Back-N window: max unACKed packets in flight */
#define TIMEOUT_MS   3000  /* ms to wait for an ACK before retransmitting; tune for your network */
#define DATA_SIZE    512   /* bytes per file-data chunk */
#define BUFSIZE      1024  /* general-purpose buffer and packet data field size */

/* Packet structure shared by client and server */
typedef struct {
    uint8_t  type;          /* one of the TYPE_* values above */
    uint32_t seq_num;       /* sequence number for reliable delivery */
    uint32_t ack_num;       /* cumulative acknowledgement number */
    uint32_t data_len;      /* number of valid bytes in the data field */
    char     data[BUFSIZE]; /* payload */
} Packet;

/* Fill in a Packet with the given fields; ack_num is left as 0 */
static void make_packet(Packet *packet, uint8_t type, uint32_t seq_num,
                        const char *data, uint32_t data_len)
{
    packet->type     = type;
    packet->seq_num  = seq_num;
    packet->ack_num  = 0;
    packet->data_len = data_len;
    if (data != NULL && data_len > 0)
        memcpy(packet->data, data, data_len);
}

/* Send a single packet; returns bytes sent or -1 on error */
static int send_packet(int sockfd, Packet *packet, struct sockaddr_in *addr)
{
    int bytes_sent = sendto(sockfd, packet, sizeof(Packet), 0,
                            (struct sockaddr *)addr, sizeof(struct sockaddr_in));
    if (bytes_sent < 0)
        perror("ERROR in sendto");
    return bytes_sent;
}

/*
 * Receive one packet, blocking for at most timeout_ms milliseconds.
 * Returns bytes received, 0 on timeout, or -1 on error.
 */
static int receive_packet_timeout(int sockfd, Packet *packet,
                                  struct sockaddr_in *addr, int timeout_ms)
{
    fd_set read_fds;
    struct timeval timeout;
    socklen_t addr_len = sizeof(struct sockaddr_in);

    FD_ZERO(&read_fds);
    FD_SET(sockfd, &read_fds);

    timeout.tv_sec  = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    int ready = select(sockfd + 1, &read_fds, NULL, NULL, &timeout);
    if (ready < 0) {
        perror("ERROR in select");
        return -1;
    } else if (ready == 0) {
        return 0; /* timeout */
    }

    int bytes_received = recvfrom(sockfd, packet, sizeof(Packet), 0,
                                  (struct sockaddr *)addr, &addr_len);
    if (bytes_received < 0)
        perror("ERROR in recvfrom");
    return bytes_received;
}

#endif
