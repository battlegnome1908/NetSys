/* 
 * udpserver.c - A simple UDP echo server 
 * usage: udpserver <port>
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


void set_timeout(int sock, int ms){
    struct timeval timeout;
    timeout.tv_sec = ms / 1000;
    timeout.tv_usec = (ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

void set_datasize(int sock, int size){
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
}

/* Reliability */
int  send_reliable(int sock, struct sockaddr_in *addr, Packet *pkt);

/* Command dispatch */
void dispatch(int sock, struct sockaddr_in *caddr, char *cmd);

/* Command handlers */
void handle_ls(int sock, struct sockaddr_in *caddr);
void handle_get(int sock, struct sockaddr_in *caddr, const char *filename);
void handle_put(int sock, struct sockaddr_in *caddr, const char *filename);
void handle_delete(int sock, struct sockaddr_in *caddr, const char *filename);
void handle_exit(int sock, struct sockaddr_in *caddr);

/*
 * error - wrapper for perror
 */
void error(char *msg) {
  perror(msg);
  exit(1);
}

int main(int argc, char **argv) {
  int sockfd; /* socket */
  int portno; /* port to listen on */
  int clientlen; /* byte size of client's address */
  struct sockaddr_in serveraddr; /* server's addr */
  struct sockaddr_in clientaddr; /* client addr */
  struct hostent *hostp; /* client host info */
  char buf[BUFSIZE]; /* message buf */
  char *hostaddrp; /* dotted decimal host addr string */
  int optval; /* flag value for setsockopt */
  int n; /* message byte size */

  /* 
   * check command line arguments 
   */
  if (argc != 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }
  portno = atoi(argv[1]);

  /* 
   * socket: create the parent socket 
   */
  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) 
    error("ERROR opening socket");

  /* setsockopt: Handy debugging trick that lets 
   * us rerun the server immediately after we kill it; 
   * otherwise we have to wait about 20 secs. 
   * Eliminates "ERROR on binding: Address already in use" error. 
   */
  optval = 1;
  setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
	     (const void *)&optval , sizeof(int));

  /*
   * build the server's Internet address
   */
  bzero((char *) &serveraddr, sizeof(serveraddr));
  serveraddr.sin_family = AF_INET;
  serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
  serveraddr.sin_port = htons((unsigned short)portno);

  /* 
   * bind: associate the parent socket with a port 
   */
  if (bind(sockfd, (struct sockaddr *) &serveraddr, 
	   sizeof(serveraddr)) < 0) 
    error("ERROR on binding");

  /* 
   * main loop: wait for a datagram, then echo it
   */
  Packet packet;
  clientlen = sizeof(clientaddr);
  while (1) {

    recvfrom(sockfd, &packet, sizeof(Packet), 0, (struct sokaddr*) &clientaddr, &clientlen);
    dispatch(sockfd, &clientaddr, packet);

    // /*
    //  * recvfrom: receive a UDP datagram from a client
    //  */
    // bzero(buf, BUFSIZE);
    // n = recvfrom(sockfd, buf, BUFSIZE, 0,
		//  (struct sockaddr *) &clientaddr, &clientlen);
    // if (n < 0)
    //   error("ERROR in recvfrom");

    // /* 
    //  * gethostbyaddr: determine who sent the datagram
    //  */
    // hostp = gethostbyaddr((const char *)&clientaddr.sin_addr.s_addr, 
		// 	  sizeof(clientaddr.sin_addr.s_addr), AF_INET);
    // if (hostp == NULL)
    //   error("ERROR on gethostbyaddr");
    // hostaddrp = inet_ntoa(clientaddr.sin_addr);
    // if (hostaddrp == NULL)
    //   error("ERROR on inet_ntoa\n");
    // printf("server received datagram from %s (%s)\n", 
	  //  hostp->h_name, hostaddrp);
    // printf("server received %d/%d bytes: %s\n", strlen(buf), n, buf);
    
    // /* 
    //  * sendto: echo the input back to the client 
    //  */
    // n = sendto(sockfd, buf, strlen(buf), 0, 
	  //      (struct sockaddr *) &clientaddr, clientlen);
    // if (n < 0) 
    //   error("ERROR in sendto");
  }
}

/* Reliability */
int  send_reliable(int sock, struct sockaddr_in *addr, Packet *pkt){
    int retries = 0;
    while (retries < MAX_RETRIES) {
        if (send_pkt(sock, pkt, addr) < 0) {
            return -1; // error in sending
        }
        Packet ack_pkt;
        int n = recv_pkt_timeout(sock, &ack_pkt, addr, TIMEOUT_MS);
        if (n < 0) {
            return -1; // error in receiving
        } else if (n == 0) {
            // timeout, retry
            retries++;
            continue;
        } else {
            // check if ACK is valid
            if (ack_pkt.type == TYPE_ACK && ack_pkt.ack == pkt->seq + pkt->data_len) {
                return 0; // success
            } else {
                // invalid ACK, retry
                retries++;
                continue;
            }
        }
    }
    return -1; // failed after max retries
}

/* Command dispatch */
void dispatch(int sock, struct sockaddr_in *caddr, Packet *pkt) {
    switch(pkt->type) {
        case TYPE_LS:     handle_ls(sock, caddr, pkt);                    break;
        case TYPE_GET:    pkt->data[pkt->data_len]='\0';
                          handle_get(sock, caddr, pkt->data, pkt);        break;
        case TYPE_PUT:    pkt->data[pkt->data_len]='\0';
                          handle_put(sock, caddr, pkt->data, pkt);        break;
        case TYPE_DELETE: pkt->data[pkt->data_len]='\0';
                          handle_delete(sock, caddr, pkt->data, pkt);     break;
        case TYPE_EXIT:   handle_exit(sock, caddr, pkt);                  break;
    }
}


/* Command handlers */
void handle_ls(int sock, struct sockaddr_in *caddr){
    DIR *d;
    struct dirent *dir;
    d = opendir(".");
    if (d) {
        char file_list[BUFSIZE] = {0};
        while ((dir = readdir(d)) != NULL) {
            if (dir->d_type == DT_REG) { // only list regular files
                strncat(file_list, dir->d_name, sizeof(file_list) - strlen(file_list) - 1);
                strncat(file_list, "\n", sizeof(file_list) - strlen(file_list) - 1);
            }
        }
        closedir(d);
        // send file list response
        Packet pkt;
        make_packet(&pkt, TYPE_LS_RESPONSE, 0, file_list, strlen(file_list));
        send_reliable(sock, caddr, &pkt);
    } else {
        perror("ERROR opening directory");
    }
}

void handle_get(int sock, struct sockaddr_in *caddr, const char *filename){
    // send GET command
    Packet pkt;
    make_packet(&pkt, TYPE_GET, 0, filename, strlen(filename));
    if (send_reliable(sock, caddr, &pkt) < 0) {
        printf("Failed to send GET command\n");
        return;
    }
    // receive file
    if (recv_file(sock, caddr, filename) < 0) {
        printf("Failed to receive file\n");
        return;
    }
}

void handle_put(int sock, struct sockaddr_in *caddr, const char *filename){
    // send PUT command
    Packet pkt;
    make_packet(&pkt, TYPE_PUT, 0, filename, strlen(filename));
    if (send_reliable(sock, caddr, &pkt) < 0) {
        printf("Failed to send PUT command\n");
        return;
    }
    // send file
    if (send_file(sock, caddr, filename) < 0) {
        printf("Failed to send file\n");
        return;
    }
}

void handle_delete(int sock, struct sockaddr_in *caddr, const char *filename){
    // send DELETE command
    Packet pkt;
    make_packet(&pkt, TYPE_DELETE, 0, filename, strlen(filename));
    if (send_reliable(sock, caddr, &pkt) < 0) {
        printf("Failed to send DELETE command\n");
        return;
    }
    // receive response
    Packet recv_pkt;
    int n = recv_pkt_timeout(sock, &recv_pkt, caddr, TIMEOUT_MS);
    if (n < 0) {
        printf("Failed to receive response\n");
        return;
    } else if (n == 0) {
        printf("Timeout while waiting for response\n");
        return;
    } else {
        if (recv_pkt.type == TYPE_DELETE_RESPONSE) {
            printf("Delete response: %s\n", recv_pkt.data);
        } else {
            printf("Unexpected packet type received\n");
        }
    }
}

void handle_exit(int sock, struct sockaddr_in *caddr){
    printf("Client requested exit\n");
}

/* File transfer */
int  send_file(int sock, struct sockaddr_in *addr, const char *path){
    // open file for reading
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        perror("ERROR opening file");
        return -1;
    }
    // send file data
    uint32_t seq = 0;
    char buffer[DATA_SIZE];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, DATA_SIZE, fp)) > 0) {
        Packet pkt;
        make_packet(&pkt, TYPE_FILE_DATA, seq, buffer, bytes_read);
        if (send_reliable(sock, addr, &pkt) < 0) {
            printf("Failed to send file data\n");
            fclose(fp);
            return -1;
        }
        seq += bytes_read;
    }
    // send end of file packet
    Packet eof_pkt;
    make_packet(&eof_pkt, TYPE_FILE_END, seq, NULL, 0);
    if (send_reliable(sock, addr, &eof_pkt) < 0) {
        printf("Failed to send end of file packet\n");
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0; // success
}

int  recv_file(int sock, struct sockaddr_in *addr, const char *path){
    // open file for writing
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        perror("ERROR opening file");
        return -1;
    }
    // receive file data
    while (1) {
        Packet pkt;
        int n = recv_pkt_timeout(sock, &pkt, addr, TIMEOUT_MS);
        if (n < 0) {
            printf("Failed to receive file data\n");
            fclose(fp);
            return -1;
        } else if (n == 0) {
            printf("Timeout while waiting for file data\n");
            fclose(fp);
            return -1;
        } else {
            if (pkt.type == TYPE_FILE_DATA) {
                fwrite(pkt.data, 1, pkt.data_len, fp);
                // send ACK
                Packet ack_pkt;
                make_packet(&ack_pkt, TYPE_ACK, 0, NULL, 0);
                ack_pkt.ack = pkt.seq + pkt.data_len;
                send_pkt(sock, &ack_pkt, addr);
            } else if (pkt.type == TYPE_FILE_END) {
                break; // end of file transfer
            } else {
                printf("Unexpected packet type received\n");
                fclose(fp);
                return -1;
            }
        }
    }
    fclose(fp);
    return 0; // success
}