#define _GNU_SOURCE
#include "http.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define buf_size 8192
#define doc_root "./www"

static int SendAll(int socket_fd, const void *buffer, size_t buffer_len) {
    const char *cursor = (const char *)buffer;
    size_t total_sent = 0;

    while (total_sent < buffer_len) {
        ssize_t sent_now = send(socket_fd, cursor + total_sent, buffer_len - total_sent, 0);
        if (sent_now < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (sent_now == 0) return -1;
        total_sent += (size_t)sent_now;
    }
    return 0;
}

static const char *ReasonPhrase(int status_code) {
    switch (status_code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 505: return "HTTP Version Not Supported";
        default:  return "Error";
    }
}

static const char *GetMimeType(const char *file_path) {
    const char *dot = strrchr(file_path, '.');
    if (!dot) return "application/octet-stream";

    char ext_lower[16];
    size_t ext_len = 0;

    dot++;  // skip '.'
    while (*dot && ext_len + 1 < sizeof(ext_lower)) {
        ext_lower[ext_len++] = (char)tolower((unsigned char)*dot++);
    }
    ext_lower[ext_len] = '\0';

    if (strcmp(ext_lower, "html") == 0 || strcmp(ext_lower, "htm") == 0) return "text/html";
    if (strcmp(ext_lower, "txt") == 0) return "text/plain";
    if (strcmp(ext_lower, "png") == 0) return "image/png";
    if (strcmp(ext_lower, "gif") == 0) return "image/gif";
    if (strcmp(ext_lower, "jpg") == 0 || strcmp(ext_lower, "jpeg") == 0) return "image/jpg";
    if (strcmp(ext_lower, "ico") == 0) return "image/x-icon";
    if (strcmp(ext_lower, "css") == 0) return "text/css";
    if (strcmp(ext_lower, "js") == 0) return "application/javascript";
    return "application/octet-stream";
}

static void SendErrorResponse(int socket_fd, const char *http_version, int status_code, bool keep_alive) {
    const char *reason = ReasonPhrase(status_code);

    char body[256];
    int body_len = snprintf(body, sizeof(body),
                            "<html><body><h1>%d %s</h1></body></html>",
                            status_code, reason);
    if (body_len < 0) body_len = 0;

    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "%s %d %s\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %d\r\n"
        "Connection: %s\r\n"
        "\r\n",
        http_version, status_code, reason,
        body_len,
        keep_alive ? "Keep-alive" : "Close"
    );

    if (header_len > 0) (void)SendAll(socket_fd, header, (size_t)header_len);
    if (body_len > 0) (void)SendAll(socket_fd, body, (size_t)body_len);
}

static bool StartsWith(const char *text, const char *prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static void ToLowerInPlace(char *text) {
    for (; *text; text++) *text = (char)tolower((unsigned char)*text);
}

// Read until we have the full header block (\r\n\r\n) or the buffer fills up.
// Returns: >0 bytes read, 0 if peer closed, -1 on error/timeout.
static int ReadHeaders(int socket_fd, char *out_buffer, size_t out_capacity) {
    size_t bytes_used = 0;
    out_buffer[0] = '\0';

    while (bytes_used + 1 < out_capacity) {
        ssize_t bytes_read = recv(socket_fd, out_buffer + bytes_used, out_capacity - 1 - bytes_used, 0);
        if (bytes_read < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (bytes_read == 0) return 0;

        bytes_used += (size_t)bytes_read;
        out_buffer[bytes_used] = '\0';

        if (strstr(out_buffer, "\r\n\r\n") != NULL) return (int)bytes_used;
        if (bytes_used >= out_capacity - 1) break;
    }

    return -1;
}

static bool ParseRequestLine(const char *request_text,
                             char *out_method, size_t method_cap,
                             char *out_uri, size_t uri_cap,
                             char *out_version, size_t version_cap) {
    const char *line_end = strstr(request_text, "\r\n");
    if (!line_end) return false;

    size_t line_len = (size_t)(line_end - request_text);
    if (line_len == 0 || line_len > 1024) return false;

    char line[1025];
    memcpy(line, request_text, line_len);
    line[line_len] = '\0';

    char *save_ptr = NULL;
    char *method_tok = strtok_r(line, " \t", &save_ptr);
    char *uri_tok = strtok_r(NULL, " \t", &save_ptr);
    char *ver_tok = strtok_r(NULL, " \t", &save_ptr);
    if (!method_tok || !uri_tok || !ver_tok) return false;

    if (strtok_r(NULL, " \t", &save_ptr) != NULL) return false;

    snprintf(out_method, method_cap, "%s", method_tok);
    snprintf(out_uri, uri_cap, "%s", uri_tok);
    snprintf(out_version, version_cap, "%s", ver_tok);
    return true;
}

static bool RequestWantsKeepAlive(const char *request_text) {
    const char *headers_end = strstr(request_text, "\r\n\r\n");
    if (!headers_end) return false;

    size_t header_bytes = (size_t)(headers_end - request_text);
    if (header_bytes > 16384) header_bytes = 16384;

    char *headers_lower = (char *)malloc(header_bytes + 1);
    if (!headers_lower) return false;

    memcpy(headers_lower, request_text, header_bytes);
    headers_lower[header_bytes] = '\0';
    ToLowerInPlace(headers_lower);

    bool wants_keep_alive = false;
    char *cursor = headers_lower;

    while ((cursor = strstr(cursor, "connection:")) != NULL) {
        char *line_end = strstr(cursor, "\r\n");
        if (!line_end) line_end = headers_lower + strlen(headers_lower);

        *line_end = '\0';
        if (strstr(cursor, "keep-alive") != NULL) {
            wants_keep_alive = true;
            break;
        }

        cursor = line_end + 1;
    }

    free(headers_lower);
    return wants_keep_alive;
}

static void StripQueryAndFragment(char *uri) {
    char *qmark = strchr(uri, '?');
    if (qmark) *qmark = '\0';

    char *hash = strchr(uri, '#');
    if (hash) *hash = '\0';
}

// Returns:
//   0  success (out_path filled)
//  -1 forbidden (bad uri / traversal / outside docroot)
//  -2 not found (safe path but doesn't exist)
#include <stdarg.h>   // add this near your includes

// snprintf wrapper: returns 0 on success, -1 if truncated or formatting failed.
static int SnprintfChecked(char *dst, size_t dst_cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, dst_cap, fmt, ap);
    va_end(ap);

    if (n < 0) return -1;
    if ((size_t)n >= dst_cap) return -1;  // would truncate
    return 0;
}

// Returns 0 on success, -1 on invalid/traversal/overflow.
static int ResolvePathUnderDocroot(const char *docroot_real,
                                  const char *uri_in,
                                  char out_path[PATH_MAX]) {
    char uri[PATH_MAX];
    if (SnprintfChecked(uri, sizeof(uri), "%s", uri_in) != 0) return -1;
    StripQueryAndFragment(uri);

    if (uri[0] != '/') return -1;

    // Simple traversal guard.
    if (strstr(uri, "..") != NULL) return -1;

    // Build "<doc_root><uri>" into a PATH_MAX buffer safely.
    char joined_path[PATH_MAX];
    if (SnprintfChecked(joined_path, sizeof(joined_path), "%s%s", DOC_ROOT, uri) != 0) return -1;

    // If it exists, validate with realpath.
    struct stat st;
    if (stat(joined_path, &st) == 0) {
        char resolved_real[PATH_MAX];
        if (!realpath(joined_path, resolved_real)) return -1;
        if (!StartsWith(resolved_real, docroot_real)) return -1;

        if (SnprintfChecked(out_path, PATH_MAX, "%s", resolved_real) != 0) return -1;
        return 0;
    }

    // If it doesn't exist, validate the parent directory is still under docroot.
    char parent_path[PATH_MAX];
    if (SnprintfChecked(parent_path, sizeof(parent_path), "%s", joined_path) != 0) return -1;

    char *last_slash = strrchr(parent_path, '/');
    if (!last_slash) return -1;

    if (last_slash == parent_path) {
        if (SnprintfChecked(parent_path, sizeof(parent_path), "%s", DOC_ROOT) != 0) return -1;
    } else {
        *last_slash = '\0';
    }

    char parent_real[PATH_MAX];
    if (!realpath(parent_path, parent_real)) return -1;
    if (!StartsWith(parent_real, docroot_real)) return -1;

    // Keep the original joined path; 404 will happen later if it doesn't exist.
    if (SnprintfChecked(out_path, PATH_MAX, "%s", joined_path) != 0) return -1;
    return 0;
}

static bool TryServeDirectoryIndex(int client_fd,
                                   const request_t *request,
                                   const char *docroot_real,
                                   const char *dir_path) {
    const char *index_names[] = {"index.html", "index.htm"};

    for (size_t i = 0; i < 2; i++) {
        char candidate_path[PATH_MAX];
        size_t dir_len = strlen(dir_path);

        int rc;
        if (dir_len > 0 && dir_path[dir_len - 1] == '/') {
            rc = SnprintfChecked(candidate_path, sizeof(candidate_path), "%s%s", dir_path, index_names[i]);
        } else {
            rc = SnprintfChecked(candidate_path, sizeof(candidate_path), "%s/%s", dir_path, index_names[i]);
        }
        if (rc != 0) {
            // Path would overflow PATH_MAX, treat as forbidden-ish.
            SendErrorResponse(client_fd, request->version, 403, request->keep_alive);
            return true;
        }

        int file_fd = -1;
        int status = 0;
        off_t file_size = 0;

        if (TryOpenFile(candidate_path, &file_fd, &file_size, &status) == 0) {
            char real_path[PATH_MAX];
            if (!realpath(candidate_path, real_path) || !StartsWith(real_path, docroot_real)) {
                close(file_fd);
                SendErrorResponse(client_fd, request->version, 403, request->keep_alive);
                return true;
            }

            ServeFile(client_fd, request->version, real_path, file_fd, file_size, request->keep_alive);
            close(file_fd);
            return true;
        }
    }

    return false;
}


static int TryOpenFile(const char *file_path, int *out_file_fd, off_t *out_file_size, int *out_status) {
    struct stat st;
    if (stat(file_path, &st) != 0) {
        *out_status = 404;
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        *out_status = 404;
        return -1;
    }

    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        *out_status = (errno == EACCES) ? 403 : 404;
        return -1;
    }

    if (fstat(file_fd, &st) != 0) {
        close(file_fd);
        *out_status = 404;
        return -1;
    }

    *out_file_fd = file_fd;
    *out_file_size = st.st_size;
    *out_status = 200;
    return 0;
}

static void ServeFile(int client_fd, const char *http_version, const char *file_path,
                      int file_fd, off_t file_size, bool keep_alive) {
    const char *mime_type = GetMimeType(file_path);

    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "%s 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lld\r\n"
        "Connection: %s\r\n"
        "\r\n",
        http_version, mime_type, (long long)file_size,
        keep_alive ? "Keep-alive" : "Close"
    );

    if (header_len > 0) (void)SendAll(client_fd, header, (size_t)header_len);

    char io_buf[buf_size];
    while (1) {
        ssize_t nread = read(file_fd, io_buf, sizeof(io_buf));
        if (nread < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (nread == 0) break;
        if (SendAll(client_fd, io_buf, (size_t)nread) < 0) break;
    }
}

// Hlepers for handling requests
typedef struct {
    char method[16];
    char uri[PATH_MAX];
    char version[16];
    bool keep_alive;
} request_t;

static bool ReadAndParseRequest(int client_fd, request_t *request) {
    char request_text[buf_size];
    int read_result = ReadHeaders(client_fd, request_text, sizeof(request_text));
    if (read_result <= 0) return false;

    if (!ParseRequestLine(request_text,
                          request->method, sizeof(request->method),
                          request->uri, sizeof(request->uri),
                          request->version, sizeof(request->version))) {
        SendErrorResponse(client_fd, "HTTP/1.1", 400, false);
        return false;
    }

    if (strcmp(request->version, "HTTP/1.0") != 0 &&
        strcmp(request->version, "HTTP/1.1") != 0) {
        SendErrorResponse(client_fd, "HTTP/1.1", 505, false);
        return false;
    }

    request->keep_alive = RequestWantsKeepAlive(request_text);
    return true;
}

static bool ValidateMethodOrReply(int client_fd, const request_t *request) {
    if (strcmp(request->method, "GET") == 0) return true;

    SendErrorResponse(client_fd, request->version, 405, request->keep_alive);
    return false;
}

static int ResolveRequestPath(const char *docroot_real, const char *uri, char resolved_path[PATH_MAX]) {
    int rc = ResolvePathUnderDocroot(docroot_real, uri, resolved_path);
    if (rc == 0) return 200;
    if (rc == -2) return 404;
    return 403;
}

static bool TryServeDirectoryIndex(int client_fd, const request_t *request, const char *docroot_real, const char *dir_path) {
    const char *index_names[] = {"index.html", "index.htm"};

    for (size_t i = 0; i < 2; i++) {
        char candidate_path[PATH_MAX];
        size_t dir_len = strlen(dir_path);

        if (dir_len > 0 && dir_path[dir_len - 1] == '/') {
            snprintf(candidate_path, sizeof(candidate_path), "%s%s", dir_path, index_names[i]);
        } else {
            snprintf(candidate_path, sizeof(candidate_path), "%s/%s", dir_path, index_names[i]);
        }

        int file_fd = -1;
        int status = 0;
        off_t file_size = 0;

        if (TryOpenFile(candidate_path, &file_fd, &file_size, &status) == 0) {
            char real_path[PATH_MAX];
            if (!realpath(candidate_path, real_path) || !StartsWith(real_path, docroot_real)) {
                close(file_fd);
                SendErrorResponse(client_fd, request->version, 403, request->keep_alive);
                return true;
            }

            ServeFile(client_fd, request->version, real_path, file_fd, file_size, request->keep_alive);
            close(file_fd);
            return true;
        }
    }

    return false;
}

static void ServeResolvedFile(int client_fd, const request_t *request, const char *docroot_real, const char *resolved_path) {
    int file_fd = -1;
    int status = 0;
    off_t file_size = 0;

    if (TryOpenFile(resolved_path, &file_fd, &file_size, &status) != 0) {
        SendErrorResponse(client_fd, request->version, status, request->keep_alive);
        return;
    }

    char real_path[PATH_MAX];
    if (!realpath(resolved_path, real_path) || !StartsWith(real_path, docroot_real)) {
        close(file_fd);
        SendErrorResponse(client_fd, request->version, 403, request->keep_alive);
        return;
    }

    ServeFile(client_fd, request->version, real_path, file_fd, file_size, request->keep_alive);
    close(file_fd);
}

void HandleSingleRequest(int client_fd, const char docroot_real[PATH_MAX], bool *out_keep_alive) {
    request_t request;
    memset(&request, 0, sizeof(request));

    if (!ReadAndParseRequest(client_fd, &request)) {
        *out_keep_alive = false;
        return;
    }

    *out_keep_alive = request.keep_alive;

    if (!ValidateMethodOrReply(client_fd, &request)) return;

    char resolved_path[PATH_MAX];
    int resolve_status = ResolveRequestPath(docroot_real, request.uri, resolved_path);
    if (resolve_status != 200) {
        SendErrorResponse(client_fd, request.version, resolve_status, request.keep_alive);
        return;
    }

    struct stat st;
    bool is_directory = (stat(resolved_path, &st) == 0 && S_ISDIR(st.st_mode));
    bool uri_ends_with_slash = (request.uri[strlen(request.uri) - 1] == '/');

    if (is_directory || uri_ends_with_slash) {
        if (TryServeDirectoryIndex(client_fd, &request, docroot_real, resolved_path)) return;
        SendErrorResponse(client_fd, request.version, 404, request.keep_alive);
        return;
    }

    ServeResolvedFile(client_fd, &request, docroot_real, resolved_path);
}
