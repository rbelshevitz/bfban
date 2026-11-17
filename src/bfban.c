/* SPDX-License-Identifier: GPL-2.0-only */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_STATE "/var/lib/bfban/banlist"
#define DEFAULT_BPFCLI "bfcli"
#define DEFAULT_CHAIN "bfban_input"
#define DEFAULT_SET "banlist"
#define DEFAULT_HOOK "BF_HOOK_NF_LOCAL_IN{priorities=101-102}"
#define MAX_KEY 64
#define MAX_BATCH 2048

struct batch_item { char address[MAX_KEY]; const char *set_name; };

static const char *env_or(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return value && *value ? value : fallback;
}

static const char *state_path(void) { return env_or("BFBAN_STATE", DEFAULT_STATE); }
static const char *bfcli_path(void) { return env_or("BFBAN_BPFCLI", DEFAULT_BPFCLI); }
static const char *chain_name(void) { return env_or("BFBAN_CHAIN", DEFAULT_CHAIN); }
static const char *hook_name(void) { return env_or("BFBAN_HOOK", DEFAULT_HOOK); }

static void usage(FILE *stream)
{
    fprintf(stream,
            "Usage: bfban init|add ADDRESS|add-many FILE|remove ADDRESS|list|flush\n"
            "Environment: BFBAN_BPFCLI, BFBAN_STATE, BFBAN_CHAIN, BFBAN_HOOK\n");
}

static const char *set_for_address(const char *address)
{
    struct in_addr v4;
    struct in6_addr v6;
    char copy[MAX_KEY];
    if (strlen(address) >= sizeof(copy)) return NULL;
    strcpy(copy, address);
    char *slash = strchr(copy, '/');
    if (slash) {
        char *end = NULL;
        long prefix;
        *slash++ = '\0';
        prefix = strtol(slash, &end, 10);
        if (*end != '\0') return NULL;
        if (inet_pton(AF_INET, copy, &v4) == 1)
            return prefix >= 0 && prefix <= 32 ? "banlist4net" : NULL;
        if (inet_pton(AF_INET6, copy, &v6) == 1)
            return prefix >= 0 && prefix <= 128 ? "banlist6net" : NULL;
        return NULL;
    }
    if (inet_pton(AF_INET, copy, &v4) == 1) return "banlist4";
    if (inet_pton(AF_INET6, copy, &v6) == 1) return "banlist6";
    return NULL;
}

static int ensure_parent(void)
{
    char path[PATH_MAX];
    const char *state = state_path();
    size_t length = strlen(state);
    if (length >= sizeof(path)) return -ENAMETOOLONG;
    memcpy(path, state, length + 1);
    char *slash = strrchr(path, '/');
    if (!slash || slash == path) return 0;
    *slash = '\0';
    if (mkdir(path, 0755) < 0 && errno != EEXIST) return -errno;
    return 0;
}

static bool state_contains(const char *address)
{
    FILE *file = fopen(state_path(), "r");
    char line[MAX_KEY + 2];
    if (!file) return false;
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strcmp(line, address) == 0) {
            fclose(file);
            return true;
        }
    }
    fclose(file);
    return false;
}

static int run_bfcli(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) return -errno;
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "bfban: exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -errno;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
    return -EIO;
}

static int write_state_with(const char *address, bool add)
{
    int r = ensure_parent();
    if (r < 0) return r;
    char temporary[PATH_MAX];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", state_path(),
                 (long)getpid()) >= (int)sizeof(temporary))
        return -ENAMETOOLONG;

    FILE *input = fopen(state_path(), "r");
    FILE *output = fopen(temporary, "w");
    if (!output) {
        if (input) fclose(input);
        return -errno;
    }
    bool found = false;
    char line[MAX_KEY + 2];
    if (input) {
        while (fgets(line, sizeof(line), input)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (!*line || strcmp(line, address) == 0) {
                if (strcmp(line, address) == 0) found = true;
                if (!add) continue;
            }
            fprintf(output, "%s\n", line);
        }
        fclose(input);
    }
    if (add && !found) fprintf(output, "%s\n", address);
    if (fclose(output) != 0 || rename(temporary, state_path()) < 0) {
        unlink(temporary);
        return -errno;
    }
    return 0;
}

static int compare_batch_items(const void *left, const void *right)
{
    const struct batch_item *a = left, *b = right;
    return strcmp(a->address, b->address);
}

static int update_set_many(const char *set_name,
                           const struct batch_item *items, size_t count)
{
    char **argv = calloc(8 + count * 2, sizeof(*argv));
    if (!argv) return -ENOMEM;
    size_t i = 0;
    argv[i++] = (char *)bfcli_path(); argv[i++] = "chain";
    argv[i++] = "update-set"; argv[i++] = "--name";
    argv[i++] = (char *)chain_name(); argv[i++] = "--set-name";
    argv[i++] = (char *)set_name;
    for (size_t n = 0; n < count; ++n) {
        argv[i++] = "--add";
        argv[i++] = (char *)items[n].address;
    }
    int r = run_bfcli(argv);
    free(argv);
    return r;
}

static int write_state_batch(const struct batch_item *items, size_t count)
{
    int r = ensure_parent();
    if (r < 0) return r;
    char temporary[PATH_MAX];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", state_path(),
                 (long)getpid()) >= (int)sizeof(temporary)) return -ENAMETOOLONG;
    FILE *input = fopen(state_path(), "r");
    FILE *output = fopen(temporary, "w");
    if (!output) { if (input) fclose(input); return -errno; }
    char line[MAX_KEY + 2];
    if (input) {
        while (fgets(line, sizeof(line), input)) fputs(line, output);
        fclose(input);
    }
    for (size_t i = 0; i < count; ++i)
        if (!state_contains(items[i].address)) fprintf(output, "%s\n", items[i].address);
    if (fclose(output) != 0 || rename(temporary, state_path()) < 0) {
        int e = errno; unlink(temporary); return -e;
    }
    return 0;
}

static int add_many(const char *path)
{
    FILE *file = fopen(path, "r");
    if (!file) return -errno;
    struct batch_item items[MAX_BATCH];
    size_t count = 0;
    char line[MAX_KEY + 128];
    while (fgets(line, sizeof(line), file)) {
        char address[MAX_KEY];
        if (sscanf(line, "%63s", address) != 1 || address[0] == '#') continue;
        const char *set_name = set_for_address(address);
        if (!set_name) {
            fclose(file);
            fprintf(stderr, "bfban: invalid address: %s\n", address);
            return -EINVAL;
        }
        if (count == MAX_BATCH) { fclose(file); return -E2BIG; }
        snprintf(items[count].address, sizeof(items[count].address), "%s", address);
        items[count++].set_name = set_name;
    }
    fclose(file);
    if (!count) return 0;
    qsort(items, count, sizeof(*items), compare_batch_items);
    size_t unique = 1;
    for (size_t i = 1; i < count; ++i)
        if (strcmp(items[i].address, items[unique - 1].address) != 0)
            items[unique++] = items[i];
    const char *sets[] = {"banlist4", "banlist4net", "banlist6", "banlist6net"};
    for (size_t s = 0; s < sizeof(sets) / sizeof(sets[0]); ++s) {
        struct batch_item group[MAX_BATCH];
        size_t group_count = 0;
        for (size_t i = 0; i < unique; ++i)
            if (strcmp(items[i].set_name, sets[s]) == 0) group[group_count++] = items[i];
        for (size_t offset = 0; offset < group_count; offset += MAX_BATCH) {
            size_t chunk = group_count - offset > MAX_BATCH ? MAX_BATCH : group_count - offset;
            int r = update_set_many(sets[s], group + offset, chunk);
            if (r < 0) return r;
        }
    }
    return write_state_batch(items, unique);
}

static int init_chain(void)
{
    char definition[PATH_MAX];
    if (snprintf(definition, sizeof(definition),
                 "chain %s %s ACCEPT set banlist4 (ip4.saddr) in {} "
                 "set banlist4net (ip4.snet) in {} set banlist6 (ip6.saddr) in {} "
                 "set banlist6net (ip6.snet) in {} rule (ip4.saddr) in banlist4 DROP "
                 "rule (ip4.snet) in banlist4net DROP rule (ip6.saddr) in banlist6 DROP "
                 "rule (ip6.snet) in banlist6net DROP",
                 chain_name(), hook_name()) >=
        (int)sizeof(definition))
        return -ENAMETOOLONG;
    char *argv[] = {(char *)bfcli_path(), "chain", "set", "--from-str",
                    definition, NULL};
    return run_bfcli(argv);
}

static int update_address(const char *address, bool add)
{
    const char *set_name = set_for_address(address);
    if (!set_name) {
        fprintf(stderr, "bfban: invalid IP address: %s\n", address);
        return -EINVAL;
    }
    char *argv[] = {(char *)bfcli_path(), "chain", "update-set", "--name",
                    (char *)chain_name(), "--set-name", (char *)set_name,
                    add ? "--add" : "--remove", (char *)address, NULL};
    int r = run_bfcli(argv);
    if (r < 0) return r;
    return write_state_with(address, add);
}

static int list_addresses(void)
{
    FILE *file = fopen(state_path(), "r");
    char line[MAX_KEY + 2];
    if (!file) {
        if (errno == ENOENT) return 0;
        return -errno;
    }
    while (fgets(line, sizeof(line), file)) fputs(line, stdout);
    fclose(file);
    return 0;
}

static int flush_all(void)
{
    char *argv[] = {(char *)bfcli_path(), "chain", "flush", "--name",
                    (char *)chain_name(), NULL};
    int r = run_bfcli(argv);
    if (r < 0) return r;
    unlink(state_path());
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    int r;
    if (strcmp(argv[1], "init") == 0) {
        r = init_chain();
    } else if ((strcmp(argv[1], "add") == 0 || strcmp(argv[1], "remove") == 0) &&
               argc == 3) {
        bool add = strcmp(argv[1], "add") == 0;
        if (!add && !state_contains(argv[2])) return 0;
        r = update_address(argv[2], add);
    } else if (strcmp(argv[1], "add-many") == 0 && argc == 3) {
        r = add_many(argv[2]);
    } else if (strcmp(argv[1], "list") == 0 && argc == 2) {
        r = list_addresses();
    } else if (strcmp(argv[1], "flush") == 0 && argc == 2) {
        r = flush_all();
    } else {
        usage(stderr);
        return 2;
    }
    if (r < 0) {
        fprintf(stderr, "bfban: %s\n", strerror(-r));
        return 1;
    }
    return 0;
}
