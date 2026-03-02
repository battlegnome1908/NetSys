#define _GNU_SOURCE
#include "http.h"

#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define backlog_size 64
#define doc_root_path "./www"
#define keepalive_timeout_sec 10

static volatile sig_atomic_t should_stop = 0;

static int listen_fd = -1; // socket so SIGINT can close it and unblock accept()

// for worker thread. Ownership transferred to thread who frees it.
typedef struct {
    int client_fd;
    char docroot_real_path[PATH_MAX];
} client_args_t;

static void OnSigInt(int signo) {
    (void)signo;
    should_stop = 1;
    if (listen_fd >= 0) close(listen_fd);
}// to unblock accept() in the main loop


static void *ClientThread(void *arg_ptr) {
    client_args_t *args = (client_args_t *)arg_ptr;

    int client_fd = args->client_fd;

    char docroot_real_path[PATH_MAX];
    snprintf(docroot_real_path, sizeof(docroot_real_path), "%s", args->docroot_real_path);
    free(args);

    struct timeval recv_timeout;
    recv_timeout.tv_sec = keepalive_timeout_sec;
    recv_timeout.tv_usec = 0;
    (void)setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));

    while (1) {
        bool keep_alive = false;
        HandleSingleRequest(client_fd, docroot_real_path, &keep_alive);
        if (!keep_alive) break;
    }

    close(client_fd);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = OnSigInt;
    sigaction(SIGINT, &sa, NULL);

    struct stat docroot_stat;
    if (stat(doc_root_path, &docroot_stat) != 0 || !S_ISDIR(docroot_stat.st_mode)) {
        fprintf(stderr, "Error: document root %s does not exist or is not a directory\n", doc_root_path);
        return 1;
    }

    char docroot_real_path[PATH_MAX];
    if (!realpath(doc_root_path, docroot_real_path)) {
        perror("realpath(doc_root_path)");
        return 1;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Error: invalid port\n");
        return 1;
    }

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    int reuse_addr = 1;
    (void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr));

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, backlog_size) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    printf("Listening on port %d (docroot=%s)\n", port, docroot_real_path);

    // Main accept loop; exits when SIGINT sets stop or listen socket is closed
    while (!should_stop) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;
        }

        client_args_t *args = (client_args_t *)malloc(sizeof(*args));
        if (!args) {
            close(client_fd);
            continue;
        }

        args->client_fd = client_fd;
        snprintf(args->docroot_real_path, sizeof(args->docroot_real_path), "%s", docroot_real_path);

        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, ClientThread, args) != 0) {
            close(client_fd);
            free(args);
            continue;
        }
        pthread_detach(thread_id);
    }

    if (listen_fd >= 0) close(listen_fd);
    return 0;
}
