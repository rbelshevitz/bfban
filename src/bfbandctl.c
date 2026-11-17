/* SPDX-License-Identifier: GPL-2.0-only */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define DEFAULT_SOCKET "/run/bfban/bfban.sock"

static const char *socket_path(void)
{
    const char *value = getenv("BFBAN_SOCKET");
    return value && *value ? value : DEFAULT_SOCKET;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 4) {
        fprintf(stderr, "Usage: bfbandctl ping|init|add IP [TTL]|remove IP|list|flush\n");
        return 2;
    }
    char request[256];
    if (strcmp(argv[1], "add") == 0 && (argc == 3 || argc == 4))
        snprintf(request, sizeof(request), "ADD %s %s", argv[2], argc == 4 ? argv[3] : "0");
    else if (strcmp(argv[1], "remove") == 0 && argc == 3)
        snprintf(request, sizeof(request), "DEL %s", argv[2]);
    else if ((strcmp(argv[1], "ping") == 0 || strcmp(argv[1], "init") == 0 ||
              strcmp(argv[1], "list") == 0 || strcmp(argv[1], "flush") == 0) && argc == 2)
        snprintf(request, sizeof(request), "%s", strcmp(argv[1], "ping") == 0 ? "PING" :
                 strcmp(argv[1], "init") == 0 ? "INIT" :
                 strcmp(argv[1], "list") == 0 ? "LIST" : "FLUSH");
    else return 2;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 1;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(socket_path()) >= sizeof(addr.sun_path)) return 1;
    strcpy(addr.sun_path, socket_path());
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "bfbandctl: %s\n", strerror(errno));
        return 1;
    }
    dprintf(fd, "%s\n", request);
    char response[1024]; ssize_t n; int failed = 0;
    while ((n = read(fd, response, sizeof(response) - 1)) > 0) {
        response[n] = '\0'; fputs(response, stdout);
        if (strncmp(response, "ERR ", 4) == 0) failed = 1;
    }
    close(fd);
    return failed ? 1 : 0;
}
