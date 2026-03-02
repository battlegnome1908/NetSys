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

// packet types
#define TYPE_CMD 0 // sends command
#define TYPE_ACK 1 // sends ack
#define TYPE_DATA 2 // data file chunk
#define TYPE_END 3 // end of transfer
#define TYPE_NOACK 4 // error or NoAcK
#define TYPE_LS 5 // LS command
#define TYPE_LS_RESPONSE 6 // LS command response with file list
#define TYPE_FILE_DATA 7 // file data chunk
#define TYPE_FILE_END 8 // end of file transfer
#define TYPE_GET 9 // GET command
#define TYPE_PUT 10 // PUT command
#define TYPE_DELETE 11 // DELETE command
#define TYPE_EXIT 12 // EXIT command
#define TYPE_DELETE_RESPONSE 13 // DELETE command response

// global variables
#define MAX_RETRIES  5
#define TIMEOUT_MS   3000 // 3 second timeout for ACKs
#define DATA_SIZE    512

#define BUFSIZE 1024

// packet struct
typedef struct {
    uint8_t  type; // from above types (TYPE_[] 1-13)
    uint32_t seq; // packet sequence number for reliability
    uint32_t ack; // which ack
    uint32_t data_len; // length of data in bytes
    char     data[BUFSIZE]; // actual data of the packet
} Packet;

// adjusts pointer passed packet struct with given info
static void make_packet(Packet *p, uint8_t type, uint32_t seq, const char *data, uint32_t len){
    p->type = type;
    p->seq = seq;
    p->ack = 0; // not used in this function
    p->data_len = len;
    if (data != NULL && len > 0) {
        memcpy(p->data, data, len);
    }
}

static int  send_pkt(int sock, Packet *p, struct sockaddr_in *addr){
    int n = sendto(sock, p, sizeof(Packet), 0, (struct sockaddr *)addr, sizeof(struct sockaddr_in));
    if (n < 0) {
        perror("ERROR in sendto");
        return -1;
    }
    return n;
}

static int  recv_pkt_timeout(int sock, Packet *p, struct sockaddr_in *addr, int ms){
    fd_set read_fds;
    struct timeval timeout;
    socklen_t addrlen = sizeof(struct sockaddr_in);

    FD_ZERO(&read_fds);
    FD_SET(sock, &read_fds);

    timeout.tv_sec = ms / 1000;
    timeout.tv_usec = (ms % 1000) * 1000;

    int n = select(sock + 1, &read_fds, NULL, NULL, &timeout);
    if (n < 0) {
        perror("ERROR in select");
        return -1;
    } else if (n == 0) {
        // timeout
        return 0;
    } else {
        // data available
        n = recvfrom(sock, p, sizeof(Packet), 0, (struct sockaddr *)addr, &addrlen);
        if (n < 0) {
            perror("ERROR in recvfrom");
            return -1;
        }
        return n;
    }
}

#endif


// note: originally this was going to hold the different ways a packet is moved (send recv, with/without reliability, etc) but I ended up putting those in the client/server files instead. This just holds the packet struct and type definitions while the makes are in the client and server.