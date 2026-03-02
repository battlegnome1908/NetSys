/* 
 * udpclient.c - A simple UDP client
 * usage: udpclient <host> <port>
 */

#include "packets.h" // defining packet struct & codes

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

// linux errors
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/select.h>


// print info
void print_prompt(void);
void parse_input(char *line, char *command, char *arg);

/* reliability */
int  send_reliable(int sock, struct sockaddr_in *addr, Packet *pkt);

/* commands */
void cmd_ls(int sock, struct sockaddr_in *addr);
void cmd_get(int sock, struct sockaddr_in *addr, const char *filename);
void cmd_put(int sock, struct sockaddr_in *addr, const char *filename);
void cmd_delete(int sock, struct sockaddr_in *addr, const char *filename);
void cmd_exit(int sock, struct sockaddr_in *addr);

/* file transfer */
int  send_file(int sock, struct sockaddr_in *addr, const char *path);
int  recv_file(int sock, struct sockaddr_in *addr, const char *path);

/*
 * error - wrapper for perror
 */
void error(char *msg) {
    perror(msg);
    exit(0);
}

int main(int argc, char **argv) {
    int sockfd, portno, n;
    int serverlen;
    struct sockaddr_in serveraddr;
    struct hostent *server;
    char *hostname;
    char buf[BUFSIZE];

    /* check command line arguments */
    if (argc != 3) {
       fprintf(stderr,"usage: %s <hostname> <port>\n", argv[0]);
       exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    /* gethostbyname: get the server's DNS entry */
    server = gethostbyname(hostname);
    if (server == NULL) {
        fprintf(stderr,"ERROR, no such host as %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)server->h_addr, 
	  (char *)&serveraddr.sin_addr.s_addr, server->h_length);
    serveraddr.sin_port = htons(portno);

    /* get a message from the user */
    bzero(buf, BUFSIZE);
    printf("Please enter msg: ");
    fgets(buf, BUFSIZE, stdin);

    /* send the message to the server */
    serverlen = sizeof(serveraddr);
    n = sendto(sockfd, buf, strlen(buf), 0, &serveraddr, serverlen);
    if (n < 0) 
      error("ERROR in sendto");
    
    /* print the server's reply */
    n = recvfrom(sockfd, buf, strlen(buf), 0, &serveraddr, &serverlen);
    if (n < 0) 
      error("ERROR in recvfrom");
    printf("Echo from server: %s", buf);
    return 0;
}


void print_prompt(void){
    printf("Enter command (ls, get <filename>, put <filename>, delete <filename>, exit): ");
}
void parse_input(char *line, char *command, char *arg){
    char *token = strtok(line, " \n");
    if (token != NULL) {
        strcpy(command, token);
        token = strtok(NULL, " \n");
        if (token != NULL) {
            strcpy(arg, token);
        } else {
            arg[0] = '\0'; // no argument
        }
    } else {
        command[0] = '\0'; // no command
        arg[0] = '\0'; // no argument
    }
}


/* reliability */
int  send_reliable(int sock, struct sockaddr_in *addr, Packet *pkt){
    // send packet and wait for ACK
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

/* commands */
void cmd_ls(int sock, struct sockaddr_in *addr){
    // send LS command
    Packet pkt;
    make_packet(&pkt, TYPE_LS, 0, NULL, 0);
    if (send_reliable(sock, addr, &pkt) < 0) {
        printf("Failed to send LS command\n");
        return;
    }
    // receive file list
    Packet recv_pkt;
    int n = recv_pkt_timeout(sock, &recv_pkt, addr, TIMEOUT_MS);
    if (n < 0) {
        printf("Failed to receive file list\n");
        return;
    } else if (n == 0) {
        printf("Timeout while waiting for file list\n");
        return;
    } else {
        if (recv_pkt.type == TYPE_LS_RESPONSE) {
            printf("Files on server:\n%s\n", recv_pkt.data);
        } else {
            printf("Unexpected packet type received\n");
        }
    }
}
void cmd_get(int sock, struct sockaddr_in *addr, const char *filename){
    // send GET command
    Packet pkt;
    make_packet(&pkt, TYPE_GET, 0, filename, strlen(filename));
    if (send_reliable(sock, addr, &pkt) < 0) {
        printf("Failed to send GET command\n");
        return;
    }
    // receive file
    if (recv_file(sock, addr, filename) < 0) {
        printf("Failed to receive file\n");
        return;
    }
}
void cmd_put(int sock, struct sockaddr_in *addr, const char *filename){
    // send PUT command
    Packet pkt;
    make_packet(&pkt, TYPE_PUT, 0, filename, strlen(filename));
    if (send_reliable(sock, addr, &pkt) < 0) {
        printf("Failed to send PUT command\n");
        return;
    }
    // send file
    if (send_file(sock, addr, filename) < 0) {
        printf("Failed to send file\n");
        return;
    }
}
void cmd_delete(int sock, struct sockaddr_in *addr, const char *filename){
    // send DELETE command
    Packet pkt;
    make_packet(&pkt, TYPE_DELETE, 0, filename, strlen(filename));
    if (send_reliable(sock, addr, &pkt) < 0) {
        printf("Failed to send DELETE command\n");
        return;
    }
    // receive response
    Packet recv_pkt;
    int n = recv_pkt_timeout(sock, &recv_pkt, addr, TIMEOUT_MS);
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
void cmd_exit(int sock, struct sockaddr_in *addr){
    // send EXIT command
    Packet pkt;
    make_packet(&pkt, TYPE_EXIT, 0, NULL, 0);
    if (send_reliable(sock, addr, &pkt) < 0) {
        printf("Failed to send EXIT command\n");
        return;
    }
    printf("Exiting client...\n");
}

/* file transfer */
int  send_file(int sock, struct sockaddr_in *addr, const char *path){
    // open file
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        perror("ERROR opening file");
        return -1;
    }
    // read file and send in chunks
    char buffer[BUFSIZE];
    uint32_t seq = 0;
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, BUFSIZE, fp)) > 0) {
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
    uint32_t expected_seq = 0;
    while (1) {
        Packet recv_pkt;
        int n = recv_pkt_timeout(sock, &recv_pkt, addr, TIMEOUT_MS);
        if (n < 0) {
            printf("Failed to receive file data\n");
            fclose(fp);
            return -1;
        } else if (n == 0) {
            printf("Timeout while waiting for file data\n");
            fclose(fp);
            return -1;
        } else {
            if (recv_pkt.type == TYPE_FILE_DATA) {
                if (recv_pkt.seq == expected_seq) {
                    fwrite(recv_pkt.data, 1, recv_pkt.data_len, fp);
                    expected_seq += recv_pkt.data_len;
                }
                // send ACK
                Packet ack_pkt;
                make_packet(&ack_pkt, TYPE_ACK, 0, NULL, 0);
                ack_pkt.ack = expected_seq; // cumulative ACK
                send_pkt(sock, &ack_pkt, addr);
            } else if (recv_pkt.type == TYPE_FILE_END) {
                break; // end of file
            } else {
                printf("Unexpected packet type received\n");
            }
        }
    }
    fclose(fp);
    return 0; // success
}