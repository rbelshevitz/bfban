/* SPDX-License-Identifier: GPL-2.0-only */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
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
#define BFBAN_PATH "/usr/sbin/bfban"
#define MAX_BANS 65536
#define MAX_KEY 64
#define BUFSIZE 65536
#define INDEX_SLOTS 131072
#define MAX_REQUEST_ITEMS 2048
#define SAVE_DURABILITY_WARNING 1

struct entry { char ip[MAX_KEY]; time_t expires; };
struct index_slot { uint64_t hash; size_t entry; bool used; };
static volatile sig_atomic_t stopping;
static bool reconciliation_needed;

#ifdef BFBAN_TEST_HOOKS
static unsigned long test_backend_calls;
static unsigned long test_save_calls;

static int test_backend_set(const char *command, const char *ip)
{
    const char *path = getenv("BFBAN_TEST_BACKEND_STATE");
    char line[MAX_KEY + 2];
    if (!path || !*path || strcmp(command, "init") == 0) return 0;
    if (strcmp(command, "flush") == 0) {
        FILE *f = fopen(path, "w");
        if (!f) return -errno;
        if (fclose(f) != 0) return -errno;
        return 0;
    }
    if (strcmp(command, "add") == 0) {
        FILE *f = fopen(path, "r");
        if (f) {
            while (fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\r\n")] = '\0';
                if (strcmp(line, ip) == 0) {
                    fclose(f);
                    return 0;
                }
            }
            fclose(f);
        } else if (errno != ENOENT) return -errno;
        f = fopen(path, "a");
        if (!f) return -errno;
        if (fprintf(f, "%s\n", ip) < 0) {
            int e = errno ? errno : EIO;
            fclose(f);
            return -e;
        }
        if (fclose(f) != 0) return -errno;
        return 0;
    }
    if (strcmp(command, "remove") == 0) {
        char temp[PATH_MAX];
        FILE *in = fopen(path, "r");
        if (!in) return errno == ENOENT ? 0 : -errno;
        if (snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid()) >=
            (int)sizeof(temp)) {
            fclose(in);
            return -ENAMETOOLONG;
        }
        FILE *out = fopen(temp, "w");
        if (!out) {
            int e = errno;
            fclose(in);
            return -e;
        }
        while (fgets(line, sizeof(line), in)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (strcmp(line, ip) != 0 && fprintf(out, "%s\n", line) < 0) {
                int e = errno ? errno : EIO;
                fclose(in); fclose(out); unlink(temp);
                return -e;
            }
        }
        int input_error = ferror(in) ? (errno ? errno : EIO) : 0;
        int output_error = fclose(out) != 0 ? (errno ? errno : EIO) : 0;
        fclose(in);
        if (input_error || output_error) {
            unlink(temp);
            return -(input_error ? input_error : output_error);
        }
        if (rename(temp, path) < 0) {
            int e = errno;
            unlink(temp);
            return -e;
        }
        return 0;
    }
    return -EINVAL;
}

static int test_number(const char *name, unsigned long *counter)
{
    const char *value = getenv(name);
    char *end = NULL;
    unsigned long target;
    if (!value || !*value) return 0;
    target = strtoul(value, &end, 10);
    if (*value == '-' || *end != '\0' || target == 0) return 0;
    ++*counter;
    return *counter == target ? -EIO : 0;
}

static int test_backend_call(const char *command, const char *ip)
{
    const char *log_path = getenv("BFBAN_TEST_BACKEND_LOG");
    if (!log_path || !*log_path) return 0;
    FILE *log = fopen(log_path, "a");
    if (!log) return -errno;
    if (fprintf(log, "%s%s%s\n", command, ip ? " " : "", ip ? ip : "") < 0) {
        int e = errno ? errno : EIO;
        fclose(log);
        return -e;
    }
    if (fclose(log) != 0) return -errno;
    const char *fail_command = getenv("BFBAN_TEST_FAIL_BACKEND_COMMAND");
    if ((!fail_command || strcmp(fail_command, command) == 0) &&
        test_number("BFBAN_TEST_FAIL_BACKEND_AT", &test_backend_calls) < 0)
        return -EIO;
    return test_backend_set(command, ip);
}
#endif

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

static int direct_update_many(const char *const *addresses, size_t count, bool add)
{
    const char *sets[] = {"banlist4", "banlist4net", "banlist6", "banlist6net"};
    enum bf_matcher_type keys[] = {BF_MATCHER_IP4_SADDR, BF_MATCHER_IP4_SNET,
                                   BF_MATCHER_IP6_SADDR, BF_MATCHER_IP6_SNET};
    for (size_t group = 0; group < 4; ++group) {
        struct bf_set *to_add = NULL, *to_remove = NULL;
        int r = bf_set_new(&to_add, sets[group], &keys[group], 1);
        if (r < 0) return r;
        r = bf_set_new(&to_remove, sets[group], &keys[group], 1);
        if (r < 0) { bf_set_free(&to_add); return r; }
        struct bf_set *delta = add ? to_add : to_remove;
        for (size_t i = 0; i < count; ++i) {
            bool prefix = strchr(addresses[i], '/') != NULL;
            bool ipv6 = strchr(addresses[i], ':') != NULL;
            if ((group == 0 && (prefix || ipv6)) || (group == 1 && (!prefix || ipv6)) ||
                (group == 2 && (prefix || !ipv6)) || (group == 3 && (!prefix || !ipv6)))
                continue;
            r = bf_set_add_elem_raw(delta, addresses[i]);
            if (r < 0) break;
        }
        if (r >= 0) r = bf_chain_update_set(env_or("BFBAN_CHAIN", "bfban_input"),
                                             to_add, to_remove);
        bf_set_free(&to_add);
        bf_set_free(&to_remove);
        if (r < 0) return r;
    }
    return 0;
}
#endif

static void signal_handler(int sig) { (void)sig; stopping = 1; }

static ssize_t read_request(int fd, char *buffer, size_t size)
{
    size_t used = 0;
    time_t deadline = time(NULL) + 2;
    while (used + 1 < size) {
        time_t remaining = deadline - time(NULL);
        if (remaining <= 0) return -ETIMEDOUT;
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int ready = poll(&pfd, 1, (int)(remaining * 1000));
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) return -errno;
        if (ready == 0) return -ETIMEDOUT;
        ssize_t n = read(fd, buffer + used, size - used - 1);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -ETIMEDOUT;
        if (n <= 0) break;
        used += (size_t)n;
        if (memchr(buffer, '\n', used)) break;
    }
    if (!memchr(buffer, '\n', used) && used + 1 >= size) return -E2BIG;
    buffer[used] = '\0';
    return used ? (ssize_t)used : -1;
}

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

static int normalize_address(const char *input, char *output, size_t size)
{
    char copy[MAX_KEY], *slash;
    struct in_addr v4;
    struct in6_addr v6;
    if (!input || strlen(input) >= sizeof(copy)) return -EINVAL;
    strcpy(copy, input);
    slash = strchr(copy, '/');
    int family = AF_UNSPEC, prefix = -1;
    if (slash) {
        char *end = NULL;
        *slash++ = '\0';
        long value = strtol(slash, &end, 10);
        if (*slash == '\0' || *end != '\0' || value < 0 || value > 128) return -EINVAL;
        prefix = (int)value;
    }
    if (inet_pton(AF_INET, copy, &v4) == 1) {
        family = AF_INET;
        if (slash && prefix > 32) return -EINVAL;
        if (slash) {
            uint32_t mask = prefix == 0 ? 0 : htonl(~0U << (32 - prefix));
            v4.s_addr &= mask;
        }
    } else if (inet_pton(AF_INET6, copy, &v6) == 1) {
        family = AF_INET6;
        if (slash) {
            for (int bit = prefix; bit < 128; ++bit)
                v6.s6_addr[bit / 8] &= (unsigned char)~(1U << (7 - bit % 8));
        }
    } else return -EINVAL;
    char address[INET6_ADDRSTRLEN];
    if (!inet_ntop(family, family == AF_INET ? (void *)&v4 : (void *)&v6,
                   address, sizeof(address))) return -errno;
    int n = slash ? snprintf(output, size, "%s/%d", address, prefix)
                  : snprintf(output, size, "%s", address);
    return n < 0 || (size_t)n >= size ? -ENAMETOOLONG : 0;
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
#ifdef BFBAN_TEST_HOOKS
    if (getenv("BFBAN_TEST_BACKEND_LOG")) return test_backend_call(command, ip);
#endif
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

static uint64_t hash_ip(const char *ip)
{
    uint64_t hash = 1469598103934665603ULL;
    while (*ip) { hash ^= (unsigned char)*ip++; hash *= 1099511628211ULL; }
    return hash ? hash : 1;
}

static int index_insert(const struct entry *entries, size_t entry,
                        struct index_slot *index)
{
    uint64_t hash = hash_ip(entries[entry].ip);
    size_t start = hash % INDEX_SLOTS;
    for (size_t i = 0; i < INDEX_SLOTS; ++i) {
        struct index_slot *slot = &index[(start + i) % INDEX_SLOTS];
        if (!slot->used) {
            slot->used = true; slot->hash = hash; slot->entry = entry;
            return 0;
        }
    }
    return -ENOSPC;
}

static void rebuild_index(const struct entry *entries, size_t count,
                          struct index_slot *index)
{
    memset(index, 0, INDEX_SLOTS * sizeof(*index));
    for (size_t n = 0; n < count; ++n) {
        size_t start = hash_ip(entries[n].ip) % INDEX_SLOTS;
        for (size_t i = 0; i < INDEX_SLOTS; ++i) {
            struct index_slot *slot = &index[(start + i) % INDEX_SLOTS];
            if (!slot->used) {
                (void)index_insert(entries, n, index);
                break;
            }
        }
    }
}

static ssize_t find(const struct entry *entries, const struct index_slot *index,
                    const char *ip)
{
    uint64_t hash = hash_ip(ip);
    size_t start = hash % INDEX_SLOTS;
    for (size_t i = 0; i < INDEX_SLOTS; ++i) {
        const struct index_slot *slot = &index[(start + i) % INDEX_SLOTS];
        if (!slot->used) return -1;
        if (slot->hash == hash && strcmp(entries[slot->entry].ip, ip) == 0)
            return (ssize_t)slot->entry;
    }
    return -1;
}

static int load(struct entry *entries, size_t *count, struct index_slot *index)
{
    FILE *f = fopen(state_path(), "r");
    char line[MAX_KEY + 64];
    *count = 0;
    memset(index, 0, INDEX_SLOTS * sizeof(*index));
    if (!f) return errno == ENOENT ? 0 : -errno;
    while (fgets(line, sizeof(line), f)) {
        long long expiry = 0;
        char ip[MAX_KEY];
        if (sscanf(line, "%63s %lld", ip, &expiry) < 1 || !valid_ip(ip)) continue;
        char normalized[MAX_KEY];
        if (normalize_address(ip, normalized, sizeof(normalized)) < 0) continue;
        strcpy(ip, normalized);
        if (*count == MAX_BANS) { fclose(f); return -ENOSPC; }
        if (find(entries, index, ip) >= 0) continue;
        strcpy(entries[*count].ip, ip);
        entries[*count].expires = (time_t)expiry;
        ++*count;
        if (index_insert(entries, *count - 1, index) < 0) {
            fclose(f); return -ENOSPC;
        }
    }
    fclose(f);
    return 0;
}

static int save(const struct entry *entries, size_t count)
{
#ifdef BFBAN_TEST_HOOKS
    const char *fail_stage = getenv("BFBAN_TEST_FAIL_SAVE_STAGE");
    bool fail_before_rename = fail_stage && strcmp(fail_stage, "before_rename") == 0;
    bool fail_after_rename = fail_stage && strcmp(fail_stage, "after_rename") == 0;
#endif
    int r = parent_dir(state_path());
    if (r < 0) return r;
    char temp[PATH_MAX];
    if (snprintf(temp, sizeof(temp), "%s.tmp.%ld", state_path(), (long)getpid()) >=
        (int)sizeof(temp)) return -ENAMETOOLONG;
    FILE *f = fopen(temp, "w");
    if (!f) return -errno;
    for (size_t i = 0; i < count; ++i) {
        if (fprintf(f, "%s %lld\n", entries[i].ip,
                    (long long)entries[i].expires) < 0) {
            int e = errno ? errno : EIO;
            fclose(f); unlink(temp); return -e;
        }
    }
    int save_error = 0;
    if (fflush(f) != 0) save_error = errno ? errno : EIO;
    if (!save_error && fsync(fileno(f)) < 0) save_error = errno ? errno : EIO;
    if (fclose(f) != 0 && !save_error) save_error = errno ? errno : EIO;
    if (save_error) { unlink(temp); return -save_error; }
#ifdef BFBAN_TEST_HOOKS
    if (test_number("BFBAN_TEST_FAIL_SAVE_AT", &test_save_calls) < 0 ||
        fail_before_rename) {
        unlink(temp);
        errno = EIO;
        return -EIO;
    }
#endif
    if (rename(temp, state_path()) < 0) { int e = errno; unlink(temp); return -e; }
#ifdef BFBAN_TEST_HOOKS
    if (fail_after_rename) return SAVE_DURABILITY_WARNING;
#endif
    char directory[PATH_MAX];
    if (strlen(state_path()) >= sizeof(directory)) {
        fprintf(stderr, "bfband: warning: state directory path is too long after rename\n");
        return SAVE_DURABILITY_WARNING;
    }
    strcpy(directory, state_path());
    char *slash = strrchr(directory, '/');
    if (slash && slash != directory) *slash = '\0';
    else strcpy(directory, ".");
    int directory_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd < 0) {
        fprintf(stderr, "bfband: warning: could not open state directory after rename: %s\n",
                strerror(errno));
        return SAVE_DURABILITY_WARNING;
    }
    int sync_result = fsync(directory_fd);
    int sync_error = errno;
    close(directory_fd);
    if (sync_result < 0) {
        fprintf(stderr, "bfband: warning: could not fsync state directory: %s\n",
                strerror(sync_error));
        return SAVE_DURABILITY_WARNING;
    }
    return 0;
}

static void mark_reconciliation_needed(const char *reason)
{
    reconciliation_needed = true;
    fprintf(stderr, "bfband: kernel/state reconciliation required: %s\n", reason);
}

static int add(const char *ip, time_t expiry, struct entry *entries, size_t *count,
               struct index_slot *index)
{
    ssize_t i = find(entries, index, ip);
    if (i >= 0) {
        time_t previous = entries[i].expires;
        entries[i].expires = expiry;
        int save_result = save(entries, *count);
        if (save_result < 0) {
            entries[i].expires = previous;
            if (save(entries, *count) < 0) {
                mark_reconciliation_needed(ip);
                fprintf(stderr, "bfband: could not restore previous state for %s\n", ip);
            }
        }
        return save_result;
    }
    if (*count >= MAX_BANS) return -ENOSPC;
    int r;
#ifdef BFBAN_WITH_LIBBPFILTER
    r = direct_enabled ? direct_update(ip, true) : run_bfban("add", ip);
#else
    r = run_bfban("add", ip);
#endif
    if (r < 0) return r;
    strcpy(entries[*count].ip, ip); entries[*count].expires = expiry; ++*count;
    if (index_insert(entries, *count - 1, index) < 0) {
        --*count;
#ifdef BFBAN_WITH_LIBBPFILTER
        if (direct_enabled) {
            if (direct_update(ip, false) < 0) mark_reconciliation_needed(ip);
        } else if (run_bfban("remove", ip) < 0) {
            mark_reconciliation_needed(ip);
        }
#else
        if (run_bfban("remove", ip) < 0) mark_reconciliation_needed(ip);
#endif
        return -ENOSPC;
    }
    r = save(entries, *count);
    if (r < 0) {
        --*count;
        rebuild_index(entries, *count, index);
#ifdef BFBAN_WITH_LIBBPFILTER
        if (direct_enabled) {
            if (direct_update(ip, false) < 0) mark_reconciliation_needed(ip);
        } else if (run_bfban("remove", ip) < 0) {
            mark_reconciliation_needed(ip);
        }
#else
        if (run_bfban("remove", ip) < 0) mark_reconciliation_needed(ip);
#endif
    }
    return r;
}

static int remove_ip(const char *ip, struct entry *entries, size_t *count,
                     struct index_slot *index)
{
    ssize_t i = find(entries, index, ip);
    if (i < 0) return 0;
    struct entry removed = entries[i];
    int r;
#ifdef BFBAN_WITH_LIBBPFILTER
    r = direct_enabled ? direct_update(ip, false) : run_bfban("remove", ip);
#else
    r = run_bfban("remove", ip);
#endif
    if (r < 0) return r;
    entries[i] = entries[*count - 1]; --*count;
    rebuild_index(entries, *count, index);
    r = save(entries, *count);
    if (r < 0) {
        entries[*count] = removed;
        ++*count;
        rebuild_index(entries, *count, index);
#ifdef BFBAN_WITH_LIBBPFILTER
        if (direct_enabled) {
            if (direct_update(ip, true) < 0) mark_reconciliation_needed(ip);
        } else if (run_bfban("add", ip) < 0) {
            mark_reconciliation_needed(ip);
        }
#else
        if (run_bfban("add", ip) < 0) mark_reconciliation_needed(ip);
#endif
        if (save(entries, *count) < 0) {
            mark_reconciliation_needed(ip);
            fprintf(stderr, "bfband: could not restore previous state for %s\n", ip);
        }
    }
    return r;
}

static int add_many(const char *const *addresses, size_t item_count,
                    struct entry *entries, size_t *count,
                    struct index_slot *index)
{
    if (!item_count) return 0;
    int r = 0;
    const char **new_addresses = calloc(item_count, sizeof(*new_addresses));
    if (!new_addresses) return -ENOMEM;
    size_t new_count = 0;
    for (size_t i = 0; i < item_count; ++i) {
        if (find(entries, index, addresses[i]) < 0) {
            if (*count + new_count >= MAX_BANS) { free(new_addresses); return -ENOSPC; }
            new_addresses[new_count++] = addresses[i];
        }
    }
    if (!new_count) { free(new_addresses); return save(entries, *count); }
#ifdef BFBAN_WITH_LIBBPFILTER
    if (direct_enabled) r = direct_update_many(new_addresses, new_count, true);
    else
#endif
    {
        for (size_t i = 0; i < new_count && r >= 0; ++i)
            r = run_bfban("add", new_addresses[i]);
    }
    if (r < 0) {
#ifdef BFBAN_WITH_LIBBPFILTER
        if (direct_enabled) {
            if (direct_update_many(new_addresses, new_count, false) < 0)
                mark_reconciliation_needed("batch add rollback");
        } else
#endif
            for (size_t i = 0; i < new_count; ++i)
                if (run_bfban("remove", new_addresses[i]) < 0)
                    mark_reconciliation_needed(new_addresses[i]);
        free(new_addresses);
        return r;
    }
    time_t expiry = 0;
    size_t previous_count = *count;
    for (size_t i = 0; i < new_count; ++i) {
        strcpy(entries[*count].ip, new_addresses[i]);
        entries[*count].expires = expiry;
        if (index_insert(entries, *count, index) < 0) {
            *count = previous_count;
            rebuild_index(entries, *count, index);
#ifdef BFBAN_WITH_LIBBPFILTER
            if (direct_enabled) {
                if (direct_update_many(new_addresses, new_count, false) < 0)
                    mark_reconciliation_needed("batch index rollback");
            } else {
                for (size_t n = 0; n < new_count; ++n)
                    if (run_bfban("remove", new_addresses[n]) < 0)
                        mark_reconciliation_needed(new_addresses[n]);
            }
#else
            for (size_t n = 0; n < new_count; ++n)
                if (run_bfban("remove", new_addresses[n]) < 0)
                    mark_reconciliation_needed(new_addresses[n]);
#endif
            if (save(entries, *count) < 0) {
                mark_reconciliation_needed("batch state restore");
                fprintf(stderr, "bfband: could not restore previous batch state\n");
            }
            free(new_addresses); return -ENOSPC;
        }
        ++*count;
    }
    r = save(entries, *count);
    if (r < 0) {
        *count = previous_count;
        rebuild_index(entries, *count, index);
#ifdef BFBAN_WITH_LIBBPFILTER
        if (direct_enabled) {
            if (direct_update_many(new_addresses, new_count, false) < 0)
                mark_reconciliation_needed("batch state rollback");
        } else {
            for (size_t i = 0; i < new_count; ++i)
                if (run_bfban("remove", new_addresses[i]) < 0)
                    mark_reconciliation_needed(new_addresses[i]);
        }
#else
        for (size_t i = 0; i < new_count; ++i)
            if (run_bfban("remove", new_addresses[i]) < 0)
                mark_reconciliation_needed(new_addresses[i]);
#endif
        if (save(entries, *count) < 0) {
            mark_reconciliation_needed("batch state restore");
            fprintf(stderr, "bfband: could not restore previous batch state\n");
        }
    }
    free(new_addresses);
    return r;
}

static int reconcile_kernel(const struct entry *entries, size_t count)
{
    int r = run_bfban("init", NULL);
    if (r < 0) return r;
    if (!count) return 0;
#ifdef BFBAN_WITH_LIBBPFILTER
    if (direct_enabled) {
        const char **addresses = calloc(count, sizeof(*addresses));
        if (!addresses) return -ENOMEM;
        for (size_t i = 0; i < count; ++i) addresses[i] = entries[i].ip;
        r = direct_update_many(addresses, count, true);
        free(addresses);
        return r;
    }
#endif
    for (size_t i = 0; i < count; ++i) {
        r = run_bfban("add", entries[i].ip);
        if (r < 0) return r;
    }
    return 0;
}

static int request(const char *line, struct entry *entries, size_t *count,
                   struct index_slot *index, char *response, size_t size)
{
    char cmd[16], ip[MAX_KEY]; long long ttl = 0;
    if (sscanf(line, "%15s", cmd) != 1) return -EINVAL;
    if (strcmp(cmd, "PING") == 0) return 1;
    if (strcmp(cmd, "INIT") == 0) {
        int reconcile_result = reconcile_kernel(entries, *count);
        if (reconcile_result >= 0) reconciliation_needed = false;
        return reconcile_result;
    }
    if (reconciliation_needed && strcmp(cmd, "FLUSH") != 0) return -EUCLEAN;
    if (strcmp(cmd, "FLUSH") == 0) {
        int r = run_bfban("flush", NULL); if (r < 0) return r;
        *count = 0;
        memset(index, 0, INDEX_SLOTS * sizeof(*index));
        r = save(entries, 0);
        if (r < 0) mark_reconciliation_needed("flush state save");
        return r;
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
    if (strcmp(cmd, "ADD_MANY") == 0) {
        char copy[BUFSIZE];
        snprintf(copy, sizeof(copy), "%s", line);
        char *saveptr = NULL;
        char *token = strtok_r(copy, " \t\r\n", &saveptr);
        const char **addresses = calloc(MAX_REQUEST_ITEMS, sizeof(*addresses));
        char (*normalized)[MAX_KEY] = calloc(MAX_REQUEST_ITEMS, sizeof(*normalized));
        if (!addresses || !normalized) { free(addresses); free(normalized); return -ENOMEM; }
        size_t item_count = 0;
        while ((token = strtok_r(NULL, " \t\r\n", &saveptr)) != NULL) {
            if (item_count == MAX_REQUEST_ITEMS) {
                free(addresses); free(normalized); return -E2BIG;
            }
            if (normalize_address(token, normalized[item_count], MAX_KEY) < 0) {
                free(addresses); free(normalized); return -EINVAL;
            }
            bool duplicate = false;
            for (size_t i = 0; i < item_count; ++i)
                if (strcmp(addresses[i], normalized[item_count]) == 0) duplicate = true;
            if (!duplicate) {
                addresses[item_count] = normalized[item_count];
                ++item_count;
            }
        }
        int result = add_many(addresses, item_count, entries, count, index);
        free(addresses); free(normalized);
        return result;
    }
    if (strcmp(cmd, "ADD") == 0 && sscanf(line, "%15s %63s %lld", cmd, ip, &ttl) >= 2) {
        if (!valid_ip(ip) || ttl < 0) return -EINVAL;
        char normalized[MAX_KEY];
        int normalize_result = normalize_address(ip, normalized, sizeof(normalized));
        if (normalize_result < 0) return normalize_result;
        return add(normalized, ttl ? time(NULL) + (time_t)ttl : 0, entries, count, index);
    }
    if (strcmp(cmd, "DEL") == 0 && sscanf(line, "%15s %63s", cmd, ip) == 2) {
        if (!valid_ip(ip)) return -EINVAL;
        char normalized[MAX_KEY];
        int normalize_result = normalize_address(ip, normalized, sizeof(normalized));
        if (normalize_result < 0) return normalize_result;
        return remove_ip(normalized, entries, count, index);
    }
    return -EINVAL;
}

int main(void)
{
    struct entry *entries = calloc(MAX_BANS, sizeof(*entries));
    struct index_slot *index = calloc(INDEX_SLOTS, sizeof(*index));
    if (!entries || !index) { free(entries); free(index); return 1; }
    size_t count;
    int r = load(entries, &count, index);
    if (r < 0) { fprintf(stderr, "bfband: could not load state: %s\n", strerror(-r)); free(entries); free(index); return 1; }
#ifdef BFBAN_WITH_LIBBPFILTER
    r = direct_setup();
    if (r < 0) { fprintf(stderr, "bfband: could not initialize kernel backend: %s\n", strerror(-r)); free(entries); free(index); return 1; }
#endif
    bool changed = false;
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].expires && entries[i].expires <= time(NULL)) {
            entries[i] = entries[count - 1];
            --count;
            --i;
            changed = true;
            continue;
        }
    }
    if (changed) {
        rebuild_index(entries, count, index);
        r = save(entries, count);
        if (r < 0) {
            fprintf(stderr, "bfband: could not persist expired-entry cleanup: %s\n",
                    strerror(-r));
            free(entries); free(index); return 1;
        }
    }
    r = reconcile_kernel(entries, count);
    if (r < 0) { fprintf(stderr, "bfband: could not reconcile kernel state: %s\n", strerror(-r)); free(entries); free(index); return 1; }
    r = parent_dir(sock_path());
    if (r < 0) { fprintf(stderr, "bfband: could not create socket directory: %s\n", strerror(-r)); free(entries); free(index); return 1; }
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) { fprintf(stderr, "bfband: socket: %s\n", strerror(errno)); free(entries); free(index); return 1; }
    unlink(sock_path());
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(sock_path()) >= sizeof(addr.sun_path)) { close(server); free(entries); free(index); return 1; }
    strcpy(addr.sun_path, sock_path());
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(server, 32) < 0) {
        fprintf(stderr, "bfband: bind/listen: %s\n", strerror(errno));
        close(server); free(entries); free(index); return 1;
    }
    chmod(sock_path(), 0660);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, signal_handler); signal(SIGINT, signal_handler);
    while (!stopping) {
        time_t now = time(NULL);
        for (size_t i = 0; i < count;) {
            if (entries[i].expires && entries[i].expires <= now) {
                int remove_result = remove_ip(entries[i].ip, entries, &count, index);
                if (remove_result < 0) {
                    fprintf(stderr, "bfband: deferred TTL removal for %s: %s\n",
                            entries[i].ip, strerror(-remove_result));
                    ++i;
                }
            } else ++i;
        }
        struct pollfd pfd = {.fd = server, .events = POLLIN};
        if (poll(&pfd, 1, 1000) <= 0) continue;
        int client = accept(server, NULL, NULL);
        if (client < 0) continue;
        struct ucred peer;
        socklen_t peer_length = sizeof(peer);
        if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &peer_length) < 0 ||
            peer.uid != 0) {
            (void)dprintf(client, "ERR Permission denied\n");
            close(client);
            continue;
        }
        char line[BUFSIZE] = {0}, response[BUFSIZE] = {0};
        ssize_t n = read_request(client, line, sizeof(line));
        int result = n > 0 ? request(line, entries, &count, index, response, sizeof(response))
                           : (n < 0 ? (int)n : -EINVAL);
        if (result == 1 && !response[0]) strcpy(response, "OK\n");
        if (result < 0) snprintf(response, sizeof(response), "ERR %s\n", strerror(-result));
        (void)dprintf(client, "%s", response);
        close(client);
    }
    close(server); unlink(sock_path());
#ifdef BFBAN_WITH_LIBBPFILTER
    if (direct_enabled) bf_ctx_teardown();
#endif
    free(entries);
    free(index);
    return 0;
}
