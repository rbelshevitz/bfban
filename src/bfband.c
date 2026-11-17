/* SPDX-License-Identifier: GPL-2.0-only */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef BFBAN_WITH_LIBBPFILTER
#include <bpfilter/bpfilter.h>
#include <bpfilter/matcher.h>
#include <bpfilter/set.h>
#endif

#define SOCKET_PATH "/run/bfban/bfban.sock"
#define STATE_PATH "/var/lib/bfban/daemon.state"
#define BFBAN_PATH "/usr/local/sbin/bfban"
#define MAX_BANS 65536
#define MAX_KEY 64
#define BUFSIZE 512

struct entry { char ip[MAX_KEY]; time_t expires; };
static volatile sig_atomic_t stopping;

#ifdef BFBAN_WITH_LIBBPFILTER
static bool direct_enabled;
#endif

static const char *env_or(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    return v && *v ? v : fallback;
}
static const char *sock_path(void) { return env_or("BFBAN_SOCKET", SOCKET_PATH); }
static const char *state_path(void) { return env_or("BFBAN_DAEMON_STATE", STATE_PATH); }
static const char *bfban_path(void) { return env_or("BFBAN_BIN", BFBAN_PATH); }

#ifdef BFBAN_WITH_LIBBPFILTER
static int direct_setup(void)
{
    const char *mode = getenv("BFBAN_DIRECT");
    if (mode && strcmp(mode, "no") == 0) return 0;
    int r = bf_ctx_setup(false, env_or("BFBAN_BPFFS", "/sys/fs/bpf"), 0);
    if (r < 0) {
        if (mode && strcmp(mode, "yes") == 0) return r;
        fprintf(stderr, "bfband: direct libbpfilter unavailable (%s), using bfcli\n",
                strerror(-r));
        return 0;
    }
    direct_enabled = true;
    return 0;
}

static int direct_update(const char *ip, bool add)
{
    enum bf_matcher_type key;
    const char *set_name;
    if (strchr(ip, '/')) {
        key = strchr(ip, ':') ? BF_MATCHER_IP6_SNET : BF_MATCHER_IP4_SNET;
        set_name = strchr(ip, ':') ? "banlist6net" : "banlist4net";
    } else {
        key = strchr(ip, ':') ? BF_MATCHER_IP6_SADDR : BF_MATCHER_IP4_SADDR;
        set_name = strchr(ip, ':') ? "banlist6" : "banlist4";
    }
    struct bf_set *to_add = NULL, *to_remove = NULL;
    int r = bf_set_new(&to_add, set_name, &key, 1);
    if (r < 0) return r;
    r = bf_set_new(&to_remove, set_name, &key, 1);
    if (r < 0) { bf_set_free(&to_add); return r; }
    struct bf_set *delta = add ? to_add : to_remove;
    r = bf_set_add_elem_raw(delta, ip);
    if (r >= 0)
        r = bf_chain_update_set(env_or("BFBAN_CHAIN", "bfban_input"),
                                to_add, to_remove);
    bf_set_free(&to_add);
    bf_set_free(&to_remove);
    return r;
}
#endif

static void signal_handler(int sig) { (void)sig; stopping = 1; }

static bool valid_ip(const char *ip)
{
    struct in_addr v4; struct in6_addr v6; char copy[MAX_KEY];
    if (strlen(ip) >= sizeof(copy)) return false;
    strcpy(copy, ip);
    char *slash = strchr(copy, '/');
    if (!slash)
        return inet_pton(AF_INET, copy, &v4) == 1 || inet_pton(AF_INET6, copy, &v6) == 1;
    char *end = NULL;
    long prefix;
    *slash++ = '\0';
    prefix = strtol(slash, &end, 10);
    if (*end != '\0') return false;
    if (inet_pton(AF_INET, copy, &v4) == 1) return prefix >= 0 && prefix <= 32;
    if (inet_pton(AF_INET6, copy, &v6) == 1) return prefix >= 0 && prefix <= 128;
    return false;
}

static int parent_dir(const char *path)
{
    char copy[PATH_MAX];
    if (strlen(path) >= sizeof(copy)) return -ENAMETOOLONG;
    strcpy(copy, path);
    char *slash = strrchr(copy, '/');
    if (!slash || slash == copy) return 0;
    *slash = '\0';
    if (mkdir(copy, 0755) < 0 && errno != EEXIST) return -errno;
    return 0;
}

static int run_bfban(const char *command, const char *ip)
{
    pid_t pid = fork();
    if (pid < 0) return -errno;
    if (pid == 0) {
        setenv("BFBAN_SOCKET", "/run/bfban/no-daemon", 1);
        char *args[] = {(char *)bfban_path(), (char *)command,
                        (char *)ip, NULL};
        if (strcmp(command, "init") == 0 || strcmp(command, "flush") == 0)
            args[2] = NULL;
        execv(args[0], args);
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) return -errno;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -EIO;
}

static int load(struct entry *entries, size_t *count)
{
    FILE *f = fopen(state_path(), "r");
    char line[MAX_KEY + 64];
    *count = 0;
    if (!f) return errno == ENOENT ? 0 : -errno;
    while (fgets(line, sizeof(line), f)) {
        long long expiry = 0;
        char ip[MAX_KEY];
        if (sscanf(line, "%63s %lld", ip, &expiry) < 1 || !valid_ip(ip)) continue;
        if (*count == MAX_BANS) { fclose(f); return -ENOSPC; }
        strcpy(entries[*count].ip, ip);
        entries[*count].expires = (time_t)expiry;
        ++*count;
    }
    fclose(f);
    return 0;
}

static int save(const struct entry *entries, size_t count)
{
    int r = parent_dir(state_path());
    if (r < 0) return r;
    char temp[PATH_MAX];
    if (snprintf(temp, sizeof(temp), "%s.tmp.%ld", state_path(), (long)getpid()) >=
        (int)sizeof(temp)) return -ENAMETOOLONG;
    FILE *f = fopen(temp, "w");
    if (!f) return -errno;
    for (size_t i = 0; i < count; ++i)
        fprintf(f, "%s %lld\n", entries[i].ip, (long long)entries[i].expires);
    if (fclose(f) != 0 || rename(temp, state_path()) < 0) {
        int e = errno; unlink(temp); return -e;
    }
    return 0;
}

static ssize_t find(const struct entry *entries, size_t count, const char *ip)
{
    for (size_t i = 0; i < count; ++i)
        if (strcmp(entries[i].ip, ip) == 0) return (ssize_t)i;
    return -1;
}

static int add(const char *ip, time_t expiry, struct entry *entries, size_t *count)
{
    ssize_t i = find(entries, *count, ip);
    if (i >= 0) { entries[i].expires = expiry; return save(entries, *count); }
    int r;
#ifdef BFBAN_WITH_LIBBPFILTER
    r = direct_enabled ? direct_update(ip, true) : run_bfban("add", ip);
#else
    r = run_bfban("add", ip);
#endif
    if (r < 0) return r;
    if (*count == MAX_BANS) return -ENOSPC;
    strcpy(entries[*count].ip, ip); entries[*count].expires = expiry; ++*count;
    return save(entries, *count);
}

static int remove_ip(const char *ip, struct entry *entries, size_t *count)
{
    ssize_t i = find(entries, *count, ip);
    if (i < 0) return 0;
    int r;
#ifdef BFBAN_WITH_LIBBPFILTER
    r = direct_enabled ? direct_update(ip, false) : run_bfban("remove", ip);
#else
    r = run_bfban("remove", ip);
#endif
    if (r < 0) return r;
    entries[i] = entries[*count - 1]; --*count;
    return save(entries, *count);
}

static int request(const char *line, struct entry *entries, size_t *count,
                   char *response, size_t size)
{
    char cmd[16], ip[MAX_KEY]; long long ttl = 0;
    if (sscanf(line, "%15s", cmd) != 1) return -EINVAL;
    if (strcmp(cmd, "PING") == 0) return 1;
    if (strcmp(cmd, "INIT") == 0) return run_bfban("init", NULL);
    if (strcmp(cmd, "FLUSH") == 0) {
        int r = run_bfban("flush", NULL); if (r < 0) return r;
        *count = 0; unlink(state_path()); return 0;
    }
    if (strcmp(cmd, "LIST") == 0) {
        size_t used = 0;
        for (size_t i = 0; i < *count; ++i) {
            int n = snprintf(response + used, size - used, "%s %lld\n", entries[i].ip,
                             (long long)entries[i].expires);
            if (n < 0 || (size_t)n >= size - used) return -ENOSPC;
            used += (size_t)n;
        }
        return 1;
    }
    if (strcmp(cmd, "ADD") == 0 && sscanf(line, "%15s %63s %lld", cmd, ip, &ttl) >= 2) {
        if (!valid_ip(ip) || ttl < 0) return -EINVAL;
        return add(ip, ttl ? time(NULL) + (time_t)ttl : 0, entries, count);
    }
    if (strcmp(cmd, "DEL") == 0 && sscanf(line, "%15s %63s", cmd, ip) == 2) {
        if (!valid_ip(ip)) return -EINVAL;
        return remove_ip(ip, entries, count);
    }
    return -EINVAL;
}

int main(void)
{
    struct entry entries[MAX_BANS]; size_t count;
    int r = load(entries, &count);
    if (r < 0) return 1;
    r = run_bfban("init", NULL);
    if (r < 0) return 1;
#ifdef BFBAN_WITH_LIBBPFILTER
    r = direct_setup();
    if (r < 0) return 1;
#endif
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].expires && entries[i].expires <= time(NULL)) {
            entries[i] = entries[count - 1];
            --count;
            --i;
            continue;
        }
        r = run_bfban("add", entries[i].ip);
        if (r < 0) return 1;
    }
    if (parent_dir(sock_path()) < 0) return 1;
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) return 1;
    unlink(sock_path());
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(sock_path()) >= sizeof(addr.sun_path)) return 1;
    strcpy(addr.sun_path, sock_path());
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(server, 32) < 0)
        return 1;
    chmod(sock_path(), 0660);
    signal(SIGTERM, signal_handler); signal(SIGINT, signal_handler);
    while (!stopping) {
        time_t now = time(NULL);
        for (size_t i = 0; i < count;) {
            if (entries[i].expires && entries[i].expires <= now)
                (void)remove_ip(entries[i].ip, entries, &count);
            else ++i;
        }
        struct pollfd pfd = {.fd = server, .events = POLLIN};
        if (poll(&pfd, 1, 1000) <= 0) continue;
        int client = accept(server, NULL, NULL);
        if (client < 0) continue;
        char line[BUFSIZE] = {0}, response[BUFSIZE] = {0};
        ssize_t n = read(client, line, sizeof(line) - 1);
        int result = n > 0 ? request(line, entries, &count, response, sizeof(response)) : -EINVAL;
        if (result == 1 && !response[0]) strcpy(response, "OK\n");
        if (result < 0) snprintf(response, sizeof(response), "ERR %s\n", strerror(-result));
        (void)dprintf(client, "%s", response);
        close(client);
    }
    close(server); unlink(sock_path());
#ifdef BFBAN_WITH_LIBBPFILTER
    if (direct_enabled) bf_ctx_teardown();
#endif
    return 0;
}
