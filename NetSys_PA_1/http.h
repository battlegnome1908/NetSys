#ifndef HTTP_H
#define HTTP_H

#include <stdbool.h>
#include <limits.h>

void HandleSingleRequest(int client_fd,
                         const char docroot_real[PATH_MAX],
                         bool *out_keep_alive);

#endif
