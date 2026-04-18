#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <signal.h>

#define BUFSIZE 8192

static char g_dir[512];

/* ------------------------------------------------------------------ helpers */

static ssize_t sendall(int fd, const void *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t r = send(fd, (const char *)buf + sent, len - sent, 0);
        if (r <= 0) return -1;
        sent += r;
    }
    return (ssize_t)len;
}

/* Read one line from fd (strips \r and \n).
   Returns line length >= 0 on success, -1 on EOF or error. */
static int readline_fd(int fd, char *buf, int maxlen) {
    int n = 0;
    char c;
    while (n < maxlen - 1) {
        ssize_t r = recv(fd, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == '\n') break;
        if (c == '\r') continue;
        buf[n++] = c;
    }
    buf[n] = '\0';
    return n;
}

/* ----------------------------------------------------------------- handlers */

/* PUT <filename> <chunk_num> <byte_count> already parsed; read data off fd. */
static void handle_put(int fd, const char *filename, int chunk_num, long size) {
    char path[768];
    snprintf(path, sizeof(path), "%s/%s.%d", g_dir, filename, chunk_num);

    FILE *f = fopen(path, "wb");
    if (!f) {
        /* drain incoming bytes so the connection stays in sync */
        char tmp[BUFSIZE];
        long rem = size;
        while (rem > 0) {
            int n = rem < BUFSIZE ? (int)rem : BUFSIZE;
            ssize_t r = recv(fd, tmp, n, 0);
            if (r <= 0) break;
            rem -= r;
        }
        sendall(fd, "ERROR\n", 6);
        return;
    }

    char buf[BUFSIZE];
    long rem = size;
    while (rem > 0) {
        int n = rem < BUFSIZE ? (int)rem : BUFSIZE;
        ssize_t r = recv(fd, buf, n, 0);
        if (r <= 0) { fclose(f); sendall(fd, "ERROR\n", 6); return; }
        fwrite(buf, 1, (size_t)r, f);
        rem -= r;
    }
    fclose(f);
    sendall(fd, "OK\n", 3);
}

/* GET <filename>: send every chunk of filename that this server holds. */
static void handle_get(int fd, const char *filename) {
    char buf[BUFSIZE];
    for (int chunk = 1; chunk <= 4; chunk++) {
        char path[768];
        snprintf(path, sizeof(path), "%s/%s.%d", g_dir, filename, chunk);

        FILE *f = fopen(path, "rb");
        if (!f) continue;

        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);

        char hdr[256];
        snprintf(hdr, sizeof(hdr), "CHUNK %d %ld\n", chunk, size);
        sendall(fd, hdr, strlen(hdr));

        long rem = size;
        while (rem > 0) {
            int n = rem < BUFSIZE ? (int)rem : BUFSIZE;
            size_t got = fread(buf, 1, (size_t)n, f);
            if (got == 0) break;
            sendall(fd, buf, got);
            rem -= (long)got;
        }
        fclose(f);
    }
    sendall(fd, "END\n", 4);
}

/* LIST: report every chunk file stored in g_dir. */
static void handle_list(int fd) {
    DIR *d = opendir(g_dir);
    if (!d) { sendall(fd, "END\n", 4); return; }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        const char *name = entry->d_name;
        const char *dot  = strrchr(name, '.');
        if (!dot || dot == name) continue;

        int cnum = atoi(dot + 1);
        if (cnum < 1 || cnum > 4) continue;

        int blen = (int)(dot - name);
        if (blen <= 0 || blen >= 256) continue;

        char base[256];
        strncpy(base, name, (size_t)blen);
        base[blen] = '\0';

        char line[512];
        snprintf(line, sizeof(line), "%s %d\n", base, cnum);
        sendall(fd, line, strlen(line));
    }
    closedir(d);
    sendall(fd, "END\n", 4);
}

/* Handle all commands from one client connection until it closes. */
static void handle_client(int fd) {
    char line[512];
    int r;
    while ((r = readline_fd(fd, line, sizeof(line))) >= 0) {
        if (r == 0) continue;  /* ignore blank lines */

        char cmd[16];
        sscanf(line, "%15s", cmd);

        if (strcmp(cmd, "PUT") == 0) {
            char filename[256];
            int  chunk_num;
            long byte_count;
            if (sscanf(line, "PUT %255s %d %ld", filename, &chunk_num, &byte_count) == 3)
                handle_put(fd, filename, chunk_num, byte_count);

        } else if (strcmp(cmd, "GET") == 0) {
            char filename[256];
            if (sscanf(line, "GET %255s", filename) == 1)
                handle_get(fd, filename);

        } else if (strcmp(cmd, "LIST") == 0) {
            handle_list(fd);
        }
    }
    close(fd);
}

/* -------------------------------------------------------------------- main */

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <directory> <port>\n", argv[0]);
        return 1;
    }

    strncpy(g_dir, argv[1], sizeof(g_dir) - 1);
    int port = atoi(argv[2]);

    /* Create storage directory if it does not exist */
    mkdir(g_dir, 0755);

    /* Let the OS reap zombie children so we don't have to waitpid */
    signal(SIGCHLD, SIG_IGN);

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(sfd, 16) < 0) {
        perror("listen"); return 1;
    }

    printf("[dfs] dir=%s port=%d\n", g_dir, port);

    while (1) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(sfd, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) { perror("accept"); continue; }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(cfd);
        } else if (pid == 0) {
            close(sfd);
            handle_client(cfd);
            exit(0);
        } else {
            close(cfd);
        }
    }

    return 0;
}
