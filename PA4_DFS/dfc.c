#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <openssl/md5.h>

#define MAX_SERVERS 8
#define MAX_FILES   1024
#define BUFSIZE     8192
#define CONF_PATH   "./dfc.conf"

typedef struct {
    char name[64];
    char host[64];
    int  port;
    int  fd;   /* TCP socket fd, -1 when not connected / unavailable */
} Server;

static Server servers[MAX_SERVERS];
static int    num_servers = 0;

/*
 * Distribution table: dist_table[x][server_idx] = { chunk_a, chunk_b }
 * Chunk numbers are 1-based.  server_idx 0 = DFS1, 1 = DFS2, etc.
 *
 *  x | DFS1    DFS2    DFS3    DFS4
 *  0 | (1,2)  (2,3)  (3,4)  (4,1)
 *  1 | (4,1)  (1,2)  (2,3)  (3,4)
 *  2 | (3,4)  (4,1)  (1,2)  (2,3)
 *  3 | (2,3)  (3,4)  (4,1)  (1,2)
 */
static const int dist_table[4][4][2] = {
    { {1,2}, {2,3}, {3,4}, {4,1} },
    { {4,1}, {1,2}, {2,3}, {3,4} },
    { {3,4}, {4,1}, {1,2}, {2,3} },
    { {2,3}, {3,4}, {4,1}, {1,2} },
};

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

static ssize_t recvall(int fd, void *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t r = recv(fd, (char *)buf + got, len - got, 0);
        if (r <= 0) return -1;
        got += r;
    }
    return (ssize_t)len;
}

/* Read one '\n'-terminated line from fd (strips \r and \n).
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

/* ------------------------------------------------------------------ config */

static void parse_conf(void) {
    FILE *f = fopen(CONF_PATH, "r");
    if (!f) {
        fprintf(stderr, "Cannot open %s\n", CONF_PATH);
        exit(1);
    }

    char line[256];
    while (fgets(line, sizeof(line), f) && num_servers < MAX_SERVERS) {
        char kw[16], name[64], hostport[128];
        if (sscanf(line, "%15s %63s %127s", kw, name, hostport) != 3) continue;
        if (strcmp(kw, "server") != 0) continue;

        /* Split "host:port" on the last colon */
        char *colon = strrchr(hostport, ':');
        if (!colon) continue;
        *colon = '\0';

        strncpy(servers[num_servers].name, name,     sizeof(servers[0].name) - 1);
        servers[num_servers].name[sizeof(servers[0].name) - 1] = '\0';
        strncpy(servers[num_servers].host, hostport, sizeof(servers[0].host) - 1);
        servers[num_servers].host[sizeof(servers[0].host) - 1] = '\0';
        servers[num_servers].port = atoi(colon + 1);
        servers[num_servers].fd   = -1;
        num_servers++;
    }
    fclose(f);
}

/* ----------------------------------------------------------------- network */

/* Attempt to connect to all configured servers with a 1-second timeout each.
   Sets servers[i].fd = -1 for any that are unreachable. */
static void connect_servers(void) {
    for (int i = 0; i < num_servers; i++) {
        servers[i].fd = -1;

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) continue;

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port   = htons((uint16_t)servers[i].port);
        if (inet_pton(AF_INET, servers[i].host, &addr.sin_addr) <= 0) {
            close(sock); continue;
        }

        /* Switch to non-blocking so connect() returns immediately */
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        int r = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
        if (r < 0 && errno != EINPROGRESS) {
            close(sock); continue;
        }

        if (r != 0) {
            /* Wait up to 1 second for the connection to complete */
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(sock, &wfds);
            struct timeval tv = {1, 0};
            if (select(sock + 1, NULL, &wfds, NULL, &tv) <= 0) {
                close(sock); continue;
            }
            int err = 0;
            socklen_t elen = sizeof(err);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen);
            if (err != 0) { close(sock); continue; }
        }

        /* Restore blocking mode for all subsequent I/O */
        fcntl(sock, F_SETFL, flags);
        servers[i].fd = sock;
    }
}

static void close_servers(void) {
    for (int i = 0; i < num_servers; i++) {
        if (servers[i].fd >= 0) {
            close(servers[i].fd);
            servers[i].fd = -1;
        }
    }
}

static int count_available(void) {
    int n = 0;
    for (int i = 0; i < num_servers; i++)
        if (servers[i].fd >= 0) n++;
    return n;
}

/* ----------------------------------------------------------------- chunking */

/* x = MD5(filename)[0] % 4 — determines chunk rotation.
   Always mod 4 because dist_table is a hardcoded 4-server layout. */
static int compute_x(const char *filename) {
    unsigned char digest[16];
    MD5((const unsigned char *)filename, strlen(filename), digest);
    return (int)(digest[0] % 4u);
}

/* Return just the filename portion of a path (no directory component). */
static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* ----------------------------------------------------------------- commands */

/* Send one PUT command (chunk cnum) over an already-open fd.
   Returns 0 on success, -1 on any error. */
static int send_chunk(int fd, const char *filename, int cnum,
                      const char *data, long size) {
    char hdr[512];
    snprintf(hdr, sizeof(hdr), "PUT %s %d %ld\n", filename, cnum, size);
    if (sendall(fd, hdr, strlen(hdr)) < 0) return -1;
    if (size > 0 && sendall(fd, data, (size_t)size) < 0) return -1;

    char resp[32];
    if (readline_fd(fd, resp, sizeof(resp)) < 0) return -1;
    return strcmp(resp, "OK") == 0 ? 0 : -1;
}

/* put: read file, split into 4 chunks, upload to all reachable servers. */
static void cmd_put(const char *filepath) {
    const char *filename = base_name(filepath);

    /* Read entire file into memory */
    FILE *f = fopen(filepath, "rb");
    if (!f) { perror(filepath); return; }

    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *data = malloc((size_t)total);
    if (!data) { fclose(f); return; }
    if (fread(data, 1, (size_t)total, f) != (size_t)total && total > 0) {
        fprintf(stderr, "Read error: %s\n", filepath);
        free(data); fclose(f); return;
    }
    fclose(f);

    connect_servers();

    /* Require all configured servers: each chunk must land on 2 distinct
       servers for the redundancy guarantee to hold. */
    if (count_available() < num_servers) {
        printf("%s put failed\n", filename);
        free(data); close_servers(); return;
    }

    /* Split file into 4 chunks (ceiling division; last chunk may be smaller) */
    long csz = (total + 3) / 4;
    char *chunks[4];
    long  sizes[4];
    for (int i = 0; i < 4; i++) {
        long offset    = (long)i * csz;
        long remaining = total - offset;
        if (remaining <= 0) {
            chunks[i] = data;   /* pointer unused when size == 0 */
            sizes[i]  = 0;
        } else {
            chunks[i] = data + offset;
            sizes[i]  = remaining < csz ? remaining : csz;
        }
    }

    int x = compute_x(filename);

    for (int i = 0; i < num_servers; i++) {
        if (servers[i].fd < 0) continue;

        int ca = dist_table[x][i][0];   /* 1-based chunk numbers */
        int cb = dist_table[x][i][1];

        /* Both PUT commands go on the same TCP connection */
        if (send_chunk(servers[i].fd, filename, ca, chunks[ca - 1], sizes[ca - 1]) < 0 ||
            send_chunk(servers[i].fd, filename, cb, chunks[cb - 1], sizes[cb - 1]) < 0) {
            printf("%s put failed\n", filename);
            free(data); close_servers(); return;
        }
    }

    free(data);
    close_servers();
}

/* Drain exactly `size` bytes from fd without allocating heap memory. */
static void drain_bytes(int fd, long size) {
    char tmp[BUFSIZE];
    long rem = size;
    while (rem > 0) {
        int n = rem < BUFSIZE ? (int)rem : BUFSIZE;
        ssize_t r = recv(fd, tmp, (size_t)n, 0);
        if (r <= 0) break;
        rem -= r;
    }
}

/* get: collect all available chunks from reachable servers, reconstruct file. */
static void cmd_get(const char *filename) {
    char *chunk_data[4] = {NULL, NULL, NULL, NULL};
    long  chunk_size[4] = {0};
    int   chunk_have[4] = {0};

    connect_servers();

    for (int i = 0; i < num_servers; i++) {
        if (servers[i].fd < 0) continue;

        char req[512];
        snprintf(req, sizeof(req), "GET %s\n", filename);
        if (sendall(servers[i].fd, req, strlen(req)) < 0) continue;

        char line[256];
        while (readline_fd(servers[i].fd, line, sizeof(line)) >= 0) {
            if (strcmp(line, "END") == 0) break;

            int  cnum;
            long csize;
            if (sscanf(line, "CHUNK %d %ld", &cnum, &csize) != 2) continue;

            if (cnum < 1 || cnum > 4 || csize < 0) {
                /* Unknown or malformed chunk: drain to keep socket in sync */
                if (csize > 0) drain_bytes(servers[i].fd, csize);
                continue;
            }

            int idx = cnum - 1;

            if (chunk_have[idx]) {
                /* Duplicate from another server: drain and discard */
                if (csize > 0) drain_bytes(servers[i].fd, csize);
                continue;
            }

            chunk_data[idx] = malloc(csize > 0 ? (size_t)csize : 1);
            if (!chunk_data[idx]) continue;

            if (csize > 0 && recvall(servers[i].fd, chunk_data[idx], (size_t)csize) < 0) {
                free(chunk_data[idx]);
                chunk_data[idx] = NULL;
                continue;
            }
            chunk_have[idx] = 1;
            chunk_size[idx] = csize;
        }
    }

    close_servers();

    /* Check that all 4 chunks arrived */
    if (!(chunk_have[0] && chunk_have[1] && chunk_have[2] && chunk_have[3])) {
        printf("%s is incomplete\n", filename);
        for (int i = 0; i < 4; i++) free(chunk_data[i]);
        return;
    }

    /* Write reconstructed file to current working directory */
    FILE *out = fopen(filename, "wb");
    if (!out) {
        perror(filename);
        for (int i = 0; i < 4; i++) free(chunk_data[i]);
        return;
    }
    for (int i = 0; i < 4; i++)
        if (chunk_size[i] > 0)
            fwrite(chunk_data[i], 1, (size_t)chunk_size[i], out);
    fclose(out);

    for (int i = 0; i < 4; i++) free(chunk_data[i]);
}

/* list: aggregate chunk bitmasks from all reachable servers, print results. */
typedef struct { char name[256]; int mask; } FileEntry;
static FileEntry file_list[MAX_FILES];
static int       file_count = 0;

static void list_add(const char *filename, int cnum) {
    /* Find existing entry or create a new one */
    for (int i = 0; i < file_count; i++) {
        if (strcmp(file_list[i].name, filename) == 0) {
            file_list[i].mask |= (1 << (cnum - 1));
            return;
        }
    }
    if (file_count >= MAX_FILES) return;
    strncpy(file_list[file_count].name, filename, sizeof(file_list[0].name) - 1);
    file_list[file_count].name[sizeof(file_list[0].name) - 1] = '\0';
    file_list[file_count].mask = (1 << (cnum - 1));
    file_count++;
}

static void cmd_list(void) {
    file_count = 0;
    connect_servers();

    for (int i = 0; i < num_servers; i++) {
        if (servers[i].fd < 0) continue;
        if (sendall(servers[i].fd, "LIST\n", 5) < 0) continue;

        char line[512];
        while (readline_fd(servers[i].fd, line, sizeof(line)) >= 0) {
            if (strcmp(line, "END") == 0) break;

            char fname[256];
            int  cnum;
            if (sscanf(line, "%255s %d", fname, &cnum) == 2
                    && cnum >= 1 && cnum <= 4)
                list_add(fname, cnum);
        }
    }

    close_servers();

    /* Print: complete if all 4 chunk bits set (0b1111 = 0xF) */
    for (int i = 0; i < file_count; i++) {
        if (file_list[i].mask == 0xF)
            printf("%s\n", file_list[i].name);
        else
            printf("%s [incomplete]\n", file_list[i].name);
    }
}

/* --------------------------------------------------------------------- main */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <list|get|put> [filename]\n", argv[0]);
        return 1;
    }

    parse_conf();

    if (strcmp(argv[1], "put") == 0) {
        if (argc < 3) { fprintf(stderr, "put requires a filename\n"); return 1; }
        cmd_put(argv[2]);

    } else if (strcmp(argv[1], "get") == 0) {
        if (argc < 3) { fprintf(stderr, "get requires a filename\n"); return 1; }
        cmd_get(argv[2]);

    } else if (strcmp(argv[1], "list") == 0) {
        cmd_list();

    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        return 1;
    }

    return 0;
}
