// Relay the prefix's authenticated loopback connections to the user's Discord socket.
#include "protocol.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
static volatile sig_atomic_t g_stopping = 0;
static volatile sig_atomic_t g_game_rpc_active = 0;

struct buffer {
    unsigned char bytes[BRIDGE_BUFFER];
    size_t start, end;
    bool eof;
};

struct connection {
    int fd[2];
    struct buffer pending[2];
    unsigned char hello[BRIDGE_HELLO_SIZE];
    size_t received;
    double deadline;
    bool control;
};

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

#ifdef BUILD_HOST_CLI
static void stop_signal(int signal_number)
{
    (void)signal_number;
    g_stopping = 1;
}
#endif

static bool configure_socket(int fd)
{
    return fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 &&
           fcntl(fd, F_SETFL, O_NONBLOCK) == 0;
}

static void close_connection(struct connection *c)
{
    for (int side = 0; side < 2; ++side) {
        if (c->fd[side] >= 0) close(c->fd[side]);
    }
    memset(c, 0, sizeof(*c));
    c->fd[0] = c->fd[1] = -1;
}

static void log_bridge(const char *fmt, ...)
{
    const char *home = getenv("HOME");
    if (!home) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/Library/Application Support/notproton/discord-rpc.log", home);
    FILE *f = fopen(path, "a");
    if (!f) return;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(f, "[%s] ", time_str);
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fprintf(f, "\n");
    fclose(f);
}

static int connect_discord(void)
{
    char user_temp[1024];
    size_t temp_size = confstr(_CS_DARWIN_USER_TEMP_DIR, user_temp, sizeof(user_temp));
    if (!temp_size || temp_size > sizeof(user_temp)) user_temp[0] = 0;

    char darwin_t[1024] = {0};
    const char *src_t = user_temp[0] ? user_temp : getenv("TMPDIR");
    if (src_t) {
        const char *t_pos = strstr(src_t, "/T/");
        if (t_pos) {
            size_t len = (t_pos - src_t) + 2; // includes "/T"
            if (len < sizeof(darwin_t)) {
                strncpy(darwin_t, src_t, len);
                darwin_t[len] = '\0';
            }
        }
    }

    const char *override = getenv("DISCORD_RPC_SOCKET_DIR");
    if (!override || !*override) override = getenv("NOTPROTON_DISCORD_SOCKET_DIR");
    const char *roots[] = {getenv("TMPDIR"), user_temp, darwin_t[0] ? darwin_t : NULL, "/tmp"};
    if (override) { roots[0] = override; roots[1] = roots[2] = roots[3] = NULL; }
    for (size_t root = 0; root < sizeof(roots) / sizeof(roots[0]); ++root) {
        if (!roots[root] || !*roots[root]) continue;
        for (int index = 0; index < 10; ++index) {
            struct sockaddr_un address = {.sun_family = AF_UNIX};
            int length = snprintf(address.sun_path, sizeof(address.sun_path),
                                  "%s/discord-ipc-%d", roots[root], index);
            if (length < 0 || length >= (int)sizeof(address.sun_path)) continue;
            struct stat info;
            if (stat(address.sun_path, &info) || !S_ISSOCK(info.st_mode) || info.st_uid != getuid()) continue;
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) continue;
            if (!configure_socket(fd)) { close(fd); continue; }
            address.sun_len = (unsigned char)(offsetof(struct sockaddr_un, sun_path) + length + 1);
            int result = connect(fd, (struct sockaddr *)&address, address.sun_len);
            if (result < 0 && errno == EINPROGRESS) {
                struct pollfd p = {.fd = fd, .events = POLLOUT};
                int error = 0;
                socklen_t size = sizeof(error);
                result = poll(&p, 1, 100);
                if (result > 0 && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) && !error)
                    result = 0;
                else result = -1;
            }
            uid_t uid;
            gid_t gid;
            if (!result && !getpeereid(fd, &uid, &gid) && uid == getuid()) {
                log_bridge("Connected to host Discord IPC socket: %s", address.sun_path);
                return fd;
            }
            close(fd);
        }
    }
    log_bridge("Warning: Could not connect to any host Discord IPC socket (checked TMPDIR, darwin /T, /tmp)");
    return -1;
}

static bool authenticate(struct connection *c, const char *token, bool *ready)
{
    ssize_t n = recv(c->fd[0], c->hello + c->received, sizeof(c->hello) - c->received, 0);
    if (n < 0) return errno == EAGAIN || errno == EINTR;
    if (!n) return false;
    c->received += (size_t)n;
    if (c->received != sizeof(c->hello)) return true;
    unsigned difference = 0;
    for (size_t i = 0; i < BRIDGE_TOKEN_LENGTH; ++i) difference |= c->hello[i] ^ (unsigned char)token[i];
    if (difference) return false;
    if (c->hello[BRIDGE_TOKEN_LENGTH] == BRIDGE_CONTROL && !*ready) {
        c->control = true;
        *ready = true;
        log_bridge("Bridge status: Named pipe \\\\.\\pipe\\discord-ipc-0 created and listening for game RPC connections.");
#ifdef BUILD_HOST_CLI
        puts("ready");
        fflush(stdout);
#endif
        return true;
    }
    if (c->hello[BRIDGE_TOKEN_LENGTH] != BRIDGE_STREAM) return false;
    log_bridge("Game connection received! Connecting to host macOS Discord socket...");
    c->fd[1] = connect_discord();
    if (c->fd[1] >= 0) {
        g_game_rpc_active = 1;
        log_bridge("Successfully bridged game RPC to host Discord! Native RPC active.");
    } else {
        log_bridge("Failed to bridge game RPC: host Discord IPC socket connection failed");
    }
    return c->fd[1] >= 0;
}

// Each direction has a bounded buffer. Stop reading a full buffer until its peer drains it.
static bool relay(struct connection *c, short events[2])
{
    for (int side = 0; side < 2; ++side) {
        struct buffer *out = &c->pending[1 - side];
        struct buffer *in = &c->pending[side];
        if (events[side] & (POLLERR | POLLNVAL)) return false;
        if ((events[side] & POLLOUT) && out->start < out->end) {
            ssize_t n = send(c->fd[side], out->bytes + out->start, out->end - out->start, 0);
            if (n < 0 && errno != EAGAIN && errno != EINTR) return false;
            if (n > 0) {
                out->start += (size_t)n;
                if (out->start == out->end) out->start = out->end = 0;
            }
        }
        if ((events[side] & POLLIN) && in->end < sizeof(in->bytes)) {
            ssize_t n = recv(c->fd[side], in->bytes + in->end, sizeof(in->bytes) - in->end, 0);
            if (n < 0 && errno != EAGAIN && errno != EINTR) return false;
            if (!n) in->eof = true;
            else if (n > 0) in->end += (size_t)n;
        }
        if (out->eof && out->start == out->end) {
            shutdown(c->fd[side], SHUT_WR);
            if (in->eof && in->start == in->end) return false;
        }
    }
    return true;
}

static void stop_child(pid_t child)
{
    if (child <= 0) return;
    for (int i = 0; i < 10; ++i) {
        pid_t result = waitpid(child, NULL, WNOHANG);
        if (result == child || (result < 0 && errno == ECHILD)) return;
        struct timespec delay = {0, 100000000};
        nanosleep(&delay, NULL);
    }
    kill(child, SIGTERM);
    for (int i = 0; i < 10; ++i) {
        pid_t result = waitpid(child, NULL, WNOHANG);
        if (result == child || (result < 0 && errno == ECHILD)) return;
        struct timespec delay = {0, 100000000};
        nanosleep(&delay, NULL);
    }
    kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
}

static int run_bridge_loop(int listener, const char *token, pid_t child, int wakeup_pipe_read, pid_t parent_pid)
{
    struct connection *clients = calloc(BRIDGE_CLIENTS + 1, sizeof(struct connection));
    if (!clients) {
        close(listener);
        if (child > 0) stop_child(child);
        return 1;
    }
    for (size_t i = 0; i < BRIDGE_CLIENTS + 1; ++i) clients[i].fd[0] = clients[i].fd[1] = -1;
    bool ready = false;
    double startup_deadline = now() + 10;
    int status = 0;

    double last_waiting_log = now();
    bool client_connected = false;

    while (!g_stopping && (parent_pid <= 0 || getppid() == parent_pid)) {
        if (child > 0 && waitpid(child, NULL, WNOHANG) == child) {
            child = -1;
            status = 1;
            break;
        }
        if (!ready && now() > startup_deadline) {
            log_bridge("Error: Bridge helper failed to report readiness within startup deadline");
            status = 1;
            break;
        }

        if (ready && !client_connected && (now() - last_waiting_log >= 15.0)) {
            last_waiting_log = now();
            log_bridge("Bridge status: Named pipe \\\\.\\pipe\\discord-ipc-0 is active and listening, but no game connection received yet. Note: If this game does not natively include Discord RPC SDK code (like vanilla Dishonored or Rain World), a community Discord RPC mod is required for the game to transmit presence.");
        }

        // fds: index 0 is listener, index 1 is wakeup pipe (if available), then client sockets
        size_t base_fds = (wakeup_pipe_read >= 0) ? 2 : 1;
        struct pollfd fds[2 + 2 * (BRIDGE_CLIENTS + 1)];
        memset(fds, 0, sizeof(fds));
        fds[0].fd = listener;
        fds[0].events = POLLIN;

        if (wakeup_pipe_read >= 0) {
            fds[1].fd = wakeup_pipe_read;
            fds[1].events = POLLIN;
        }

        for (size_t i = 0; i < BRIDGE_CLIENTS + 1; ++i) {
            struct connection *c = &clients[i];
            for (int side = 0; side < 2; ++side) {
                struct pollfd *p = &fds[base_fds + 2 * i + side];
                p->fd = c->fd[side];
                if (!c->pending[side].eof && c->pending[side].end < BRIDGE_BUFFER) p->events |= POLLIN;
                if (c->pending[1 - side].start < c->pending[1 - side].end) p->events |= POLLOUT;
                if (!p->events) p->fd = -1;
            }
        }

        size_t total_poll_fds = base_fds + 2 * (BRIDGE_CLIENTS + 1);
        if (poll(fds, (nfds_t)total_poll_fds, 100) < 0 && errno != EINTR) {
            status = 1;
            break;
        }

        if (wakeup_pipe_read >= 0 && (fds[1].revents & POLLIN)) {
            break; // Stop requested via wakeup pipe
        }

        for (size_t i = 0; i < BRIDGE_CLIENTS + 1; ++i) {
            struct connection *c = &clients[i];
            if (c->fd[0] < 0) continue;
            short events[] = {fds[base_fds + 2 * i].revents, fds[base_fds + 2 * i + 1].revents};
            bool alive = true;
            if (c->received < BRIDGE_HELLO_SIZE) {
                if (now() > c->deadline) alive = false;
                else if (events[0]) {
                    alive = authenticate(c, token, &ready);
                    if (alive && c->fd[1] >= 0) client_connected = true;
                }
            } else if (c->control) {
                if (events[0]) { g_stopping = 1; alive = false; }
            } else alive = relay(c, events);
            if (!alive) close_connection(c);
        }

        if (fds[0].revents & POLLIN) {
            int fd = accept(listener, NULL, NULL);
            if (fd >= 0) {
                size_t slot = 0;
                while (slot < BRIDGE_CLIENTS + 1 && clients[slot].fd[0] >= 0) ++slot;
                if (slot == BRIDGE_CLIENTS + 1 || !configure_socket(fd)) {
                    close(fd);
                } else {
                    clients[slot].fd[0] = fd;
                    clients[slot].deadline = now() + 2;
                }
            }
        }
    }

    close(listener);
    for (size_t i = 0; i < BRIDGE_CLIENTS + 1; ++i) close_connection(&clients[i]);
    free(clients);
    if (child > 0) stop_child(child);
    return status;
}

// -----------------------------------------------------------------------------
// AUTO-PRESENCE (Automatic Steam game detection & Discord activity broadcast)
// -----------------------------------------------------------------------------
#ifndef BUILD_HOST_CLI
static int write_ipc_frame(int fd, uint32_t opcode, const char *payload)
{
    if (fd < 0 || !payload) return -1;
    uint32_t len = (uint32_t)strlen(payload);
    uint32_t header[2] = {opcode, len};
    size_t written = 0;
    while (written < sizeof(header)) {
        struct pollfd p = {.fd = fd, .events = POLLOUT};
        if (poll(&p, 1, 2000) <= 0) return -1;
        ssize_t n = send(fd, ((char *)header) + written, sizeof(header) - written, 0);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n <= 0) return -1;
        written += (size_t)n;
    }
    written = 0;
    while (written < len) {
        struct pollfd p = {.fd = fd, .events = POLLOUT};
        if (poll(&p, 1, 2000) <= 0) return -1;
        ssize_t n = send(fd, payload + written, len - written, 0);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n <= 0) return -1;
        written += (size_t)n;
    }
    return 0;
}

static int read_ipc_frame(int fd, uint32_t *opcode, char *buffer, size_t bufsize)
{
    if (fd < 0 || !buffer || bufsize < 8) return -1;
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (poll(&p, 1, 2500) <= 0) return -1;
    uint32_t header[2];
    size_t read_bytes = 0;
    while (read_bytes < sizeof(header)) {
        if (read_bytes > 0 && poll(&p, 1, 1000) <= 0) return -1;
        ssize_t n = recv(fd, ((char *)header) + read_bytes, sizeof(header) - read_bytes, 0);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n <= 0) return -1;
        read_bytes += (size_t)n;
    }
    *opcode = header[0];
    uint32_t payload_len = header[1];
    uint32_t to_read = payload_len < (bufsize - 1) ? payload_len : (uint32_t)(bufsize - 1);
    read_bytes = 0;
    while (read_bytes < to_read) {
        if (poll(&p, 1, 1000) <= 0) return -1;
        ssize_t n = recv(fd, buffer + read_bytes, to_read - read_bytes, 0);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n <= 0) return -1;
        read_bytes += (size_t)n;
    }
    buffer[read_bytes] = '\0';
    return 0;
}

static void json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 2 < dst_size; ++i) {
        if (src[i] == '\"' || src[i] == '\\') {
            dst[o++] = '\\';
            dst[o++] = src[i];
        } else if ((unsigned char)src[i] >= 32 && (unsigned char)src[i] <= 126) {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
}

static const char *getenv_alias(const char *primary, const char *legacy)
{
    const char *v = getenv(primary);
    return v ? v : getenv(legacy);
}

static void extract_appid(const char *wine_path, char *out_appid, size_t size)
{
    const char *custom_id = getenv_alias("DISCORD_RPC_STEAM_APPID", "NOTPROTON_DISCORD_APPID");
    if (custom_id && *custom_id) {
        strncpy(out_appid, custom_id, size - 1);
        out_appid[size - 1] = '\0';
        return;
    }
    const char *env_id = getenv("SteamAppId");
    if (!env_id || !*env_id) env_id = getenv("STEAM_COMPAT_APP_ID");
    if (env_id && *env_id) {
        strncpy(out_appid, env_id, size - 1);
        out_appid[size - 1] = '\0';
        return;
    }
    if (wine_path) {
        const char *p = strstr(wine_path, "/launchers/");
        if (p) {
            p += 11;
            size_t idx = 0;
            while (*p >= '0' && *p <= '9' && idx < size - 1) {
                out_appid[idx++] = *p++;
            }
            out_appid[idx] = '\0';
            if (out_appid[0]) return;
        }
    }
    out_appid[0] = '\0';
}

static bool dir_exists(const char *path)
{
    struct stat st;
    return path && *path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void resolve_steam_root(char *out_root, size_t out_size)
{
    const char *client_path = getenv("STEAM_COMPAT_CLIENT_INSTALL_PATH");
    if (client_path && client_path[0] == '/') {
        char buf[1024];
        strncpy(buf, client_path, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        for (int level = 0; level < 8; ++level) {
            char probe[1088];
            snprintf(probe, sizeof(probe), "%s/steamapps", buf);
            if (dir_exists(probe)) {
                strncpy(out_root, buf, out_size - 1);
                out_root[out_size - 1] = '\0';
                return;
            }
            snprintf(probe, sizeof(probe), "%s/userdata", buf);
            if (dir_exists(probe)) {
                strncpy(out_root, buf, out_size - 1);
                out_root[out_size - 1] = '\0';
                return;
            }
            char *slash = strrchr(buf, '/');
            if (!slash || slash == buf) break;
            *slash = '\0';
        }
    }
    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(out_root, out_size, "%s/Library/Application Support/Steam", home);
    } else {
        out_root[0] = '\0';
    }
}

static bool read_appmanifest_name(const char *steamapps_dir, const char *appid, char *out_name, size_t out_size)
{
    char manifest_path[1024];
    snprintf(manifest_path, sizeof(manifest_path), "%s/appmanifest_%s.acf", steamapps_dir, appid);
    FILE *f = fopen(manifest_path, "r");
    if (!f) return false;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "\"name\"");
        if (p) {
            p += 6;
            while (*p && (*p == ' ' || *p == '\t' || *p == '\"')) ++p;
            char *end = strchr(p, '\"');
            if (end) {
                *end = '\0';
                strncpy(out_name, p, out_size - 1);
                out_name[out_size - 1] = '\0';
                fclose(f);
                return true;
            }
        }
    }
    fclose(f);
    return false;
}

static bool find_name_in_libraryfolders(const char *steam_root, const char *appid, char *out_name, size_t out_size)
{
    char vdf_path[1024];
    snprintf(vdf_path, sizeof(vdf_path), "%s/steamapps/libraryfolders.vdf", steam_root);
    FILE *f = fopen(vdf_path, "r");
    if (!f) return false;

    char line[1280];
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "\"path\"");
        if (!p) continue;
        p += 6;
        while (*p && (*p == ' ' || *p == '\t' || *p == '\"')) ++p;
        char *end = strchr(p, '\"');
        if (!end || end == p) continue;
        *end = '\0';

        // Unescape doubled backslashes if present
        char lib_path[1024];
        size_t o = 0;
        for (char *s = p; *s && o + 1 < sizeof(lib_path); ++s) {
            if (s[0] == '\\' && s[1] == '\\') ++s;
            lib_path[o++] = *s;
        }
        lib_path[o] = '\0';

        char steamapps[1088];
        snprintf(steamapps, sizeof(steamapps), "%s/steamapps", lib_path);
        if (read_appmanifest_name(steamapps, appid, out_name, out_size)) {
            fclose(f);
            return true;
        }
    }
    fclose(f);
    return false;
}

static bool parse_shortcuts_vdf(const unsigned char **cursor, const unsigned char *end,
                                uint32_t wanted_id, unsigned depth,
                                char *out_name, size_t out_size)
{
    char title[256] = {0};
    uint32_t id = 0;
    bool has_id = false;
    while (*cursor < end) {
        unsigned type = *(*cursor)++;
        if (type == 8) {
            if (depth == 2 && has_id && id == wanted_id && title[0]) {
                strncpy(out_name, title, out_size - 1);
                out_name[out_size - 1] = '\0';
                return true;
            }
            return false;
        }
        const unsigned char *key = *cursor;
        const unsigned char *zero = memchr(key, 0, (size_t)(end - key));
        if (!zero) break;
        *cursor = zero + 1;
        if (type == 0 && depth < 8) {
            if (parse_shortcuts_vdf(cursor, end, wanted_id, depth + 1, out_name, out_size)) {
                return true;
            }
        } else if (type == 1) {
            const unsigned char *val = *cursor;
            zero = memchr(val, 0, (size_t)(end - val));
            if (!zero) break;
            if (depth == 2 && strcasecmp((const char *)key, "appname") == 0) {
                size_t len = (size_t)(zero - val);
                if (len >= sizeof(title)) len = sizeof(title) - 1;
                memcpy(title, val, len);
                title[len] = '\0';
            }
            *cursor = zero + 1;
        } else if (type == 2 && (end - *cursor) >= 4) {
            if (depth == 2 && strcasecmp((const char *)key, "appid") == 0) {
                memcpy(&id, *cursor, 4);
                has_id = true;
            }
            *cursor += 4;
        } else {
            break;
        }
    }
    *cursor = end;
    return false;
}

static bool find_shortcut_name(const char *steam_root, uint32_t wanted_id, char *out_name, size_t out_size)
{
    char userdata_dir[1024];
    snprintf(userdata_dir, sizeof(userdata_dir), "%s/userdata", steam_root);
    DIR *d = opendir(userdata_dir);
    if (!d) return false;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char vdf_path[1280];
        snprintf(vdf_path, sizeof(vdf_path), "%s/%s/config/shortcuts.vdf", userdata_dir, entry->d_name);
        FILE *f = fopen(vdf_path, "rb");
        if (!f) continue;

        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz <= 4 * 1024 * 1024) {
            unsigned char *buf = malloc((size_t)sz);
            if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz) {
                const unsigned char *cur = buf;
                if (parse_shortcuts_vdf(&cur, buf + sz, wanted_id, 0, out_name, out_size)) {
                    free(buf);
                    fclose(f);
                    closedir(d);
                    return true;
                }
            }
            free(buf);
        }
        fclose(f);
    }
    closedir(d);
    return false;
}

static bool get_game_name_for_appid(const char *appid, char *out_name, size_t out_size)
{
    const char *home = getenv("HOME");
    char steam_root[1024] = {0};
    resolve_steam_root(steam_root, sizeof(steam_root));

    if (steam_root[0] && appid && *appid) {
        // 1. Check primary steamapps + external Steam libraries in libraryfolders.vdf
        char steamapps[1088];
        snprintf(steamapps, sizeof(steamapps), "%s/steamapps", steam_root);
        if (read_appmanifest_name(steamapps, appid, out_name, out_size)) return true;
        if (find_name_in_libraryfolders(steam_root, appid, out_name, out_size)) return true;
    }

    // 2. Check non-Steam shortcut title in userdata/*/config/shortcuts.vdf
    const char *sgid = getenv("SteamGameId");
    if (steam_root[0] && sgid && *sgid) {
        unsigned long long gid = strtoull(sgid, NULL, 10);
        if ((gid & 0xFFFFFFFFULL) == 0x02000000ULL) {
            uint32_t shortcut_id = (uint32_t)(gid >> 32);
            if (find_shortcut_name(steam_root, shortcut_id, out_name, out_size)) return true;
        }
    }

    if (home && appid && *appid) {
        // 3. Check NotProton launchers/<appid>/runner/.../wine.app/Contents/Info.plist CFBundleName
        char plist_path[1024];
        snprintf(plist_path, sizeof(plist_path),
                 "%s/Library/Application Support/notproton/launchers/%s/runner/lib/wine/x86_64-unix/wine.app/Contents/Info.plist",
                 home, appid);
        FILE *f = fopen(plist_path, "r");
        if (f) {
            char line[1024];
            while (fgets(line, sizeof(line), f)) {
                char *p = strstr(line, "<key>CFBundleName</key>");
                if (p && fgets(line, sizeof(line), f)) {
                    char *start = strstr(line, "<string>");
                    if (start) {
                        start += 8;
                        char *end = strstr(start, "</string>");
                        if (end) {
                            *end = '\0';
                            strncpy(out_name, start, out_size - 1);
                            out_name[out_size - 1] = '\0';
                            fclose(f);
                            return true;
                        }
                    }
                }
            }
            fclose(f);
        }
    }

    // 4. Fallback to STEAM_COMPAT_INSTALL_PATH folder basename (for non-Steam shortcuts)
    const char *install_path = getenv("STEAM_COMPAT_INSTALL_PATH");
    if (install_path && *install_path) {
        char buf[1024];
        strncpy(buf, install_path, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        size_t len = strlen(buf);
        while (len > 1 && buf[len - 1] == '/') buf[--len] = '\0';
        const char *slash = strrchr(buf, '/');
        const char *base = slash ? slash + 1 : buf;
        if (*base && strcmp(base, "common") != 0 && strcmp(base, "/") != 0) {
            strncpy(out_name, base, out_size - 1);
            out_name[out_size - 1] = '\0';
            return true;
        }
    }

    return false;
}

static bool get_game_icon_hash(const char *appid, char *out_hash, size_t out_size)
{
    char steam_root[1024] = {0};
    resolve_steam_root(steam_root, sizeof(steam_root));
    if (!steam_root[0] || !appid || !*appid) return false;

    char dir_path[1088];
    snprintf(dir_path, sizeof(dir_path), "%s/appcache/librarycache/%s", steam_root, appid);
    DIR *d = opendir(dir_path);
    if (!d) return false;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len == 44 && strcmp(entry->d_name + 40, ".jpg") == 0) {
            bool is_hex = true;
            for (int i = 0; i < 40; ++i) {
                char c = entry->d_name[i];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                    is_hex = false;
                    break;
                }
            }
            if (is_hex) {
                if (out_size > 40) {
                    strncpy(out_hash, entry->d_name, 40);
                    out_hash[40] = '\0';
                    closedir(d);
                    return true;
                }
            }
        }
    }
    closedir(d);
    return false;
}

struct bridge_config {
    bool has_state;
    char state[256];
    bool has_art;
    char art[32];
    bool has_force_auto;
    bool force_auto;
    bool has_disable_auto;
    bool disable_auto;
    bool has_wait_sec;
    int wait_sec;
    bool has_socket_dir;
    char socket_dir[1024];
    bool has_client_id;
    char client_id[32];
    bool has_small_image;
    char small_image[512];
};

static char *trim_whitespace(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') ++s;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
    return s;
}

static void load_bridge_config(const char *plugin_dir, struct bridge_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    if (!plugin_dir || !*plugin_dir) return;

    char path[1024];
    snprintf(path, sizeof(path), "%s/config.conf", plugin_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[1280];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim_whitespace(line);
        if (!*p || *p == '#' || *p == ';') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';

        char *key = trim_whitespace(p);
        char *val = trim_whitespace(eq + 1);

        // Strip matching surrounding quotes ("..." or '...')
        size_t vlen = strlen(val);
        if (vlen >= 2 && ((val[0] == '\"' && val[vlen - 1] == '\"') ||
                          (val[0] == '\'' && val[vlen - 1] == '\''))) {
            val[vlen - 1] = '\0';
            ++val;
        }

        if (strcmp(key, "subtitle") == 0 || strcmp(key, "state") == 0) {
            cfg->has_state = true;
            strncpy(cfg->state, val, sizeof(cfg->state) - 1);
            cfg->state[sizeof(cfg->state) - 1] = '\0';
        } else if ((strcmp(key, "artwork") == 0 || strcmp(key, "art") == 0) && *val) {
            cfg->has_art = true;
            strncpy(cfg->art, val, sizeof(cfg->art) - 1);
            cfg->art[sizeof(cfg->art) - 1] = '\0';
        } else if (strcmp(key, "mode") == 0 && *val) {
            if (strcmp(val, "fallback") == 0 || strcmp(val, "instant") == 0) {
                cfg->has_force_auto = true;
                cfg->force_auto = true;
                cfg->has_disable_auto = true;
                cfg->disable_auto = false;
            } else if (strcmp(val, "native_only") == 0) {
                cfg->has_disable_auto = true;
                cfg->disable_auto = true;
                cfg->has_force_auto = true;
                cfg->force_auto = false;
            } else if (strcmp(val, "auto") == 0) {
                cfg->has_force_auto = true;
                cfg->force_auto = false;
                cfg->has_disable_auto = true;
                cfg->disable_auto = false;
            }
        } else if (strcmp(key, "force_auto") == 0 && *val) {
            cfg->has_force_auto = true;
            cfg->force_auto = (strcmp(val, "0") != 0 && strcmp(val, "false") != 0);
        } else if (strcmp(key, "disable_auto") == 0 && *val) {
            cfg->has_disable_auto = true;
            cfg->disable_auto = (strcmp(val, "0") != 0 && strcmp(val, "false") != 0);
        } else if ((strcmp(key, "fallback_delay") == 0 || strcmp(key, "wait_sec") == 0) && *val) {
            int w = atoi(val);
            if (w >= 0 && w <= 60) {
                cfg->has_wait_sec = true;
                cfg->wait_sec = w;
            }
        } else if (strcmp(key, "socket_dir") == 0 && *val) {
            cfg->has_socket_dir = true;
            strncpy(cfg->socket_dir, val, sizeof(cfg->socket_dir) - 1);
            cfg->socket_dir[sizeof(cfg->socket_dir) - 1] = '\0';
        } else if (strcmp(key, "client_id") == 0 && *val) {
            cfg->has_client_id = true;
            strncpy(cfg->client_id, val, sizeof(cfg->client_id) - 1);
            cfg->client_id[sizeof(cfg->client_id) - 1] = '\0';
        } else if (strcmp(key, "small_image") == 0 && *val) {
            cfg->has_small_image = true;
            strncpy(cfg->small_image, val, sizeof(cfg->small_image) - 1);
            cfg->small_image[sizeof(cfg->small_image) - 1] = '\0';
        }
    }
    fclose(f);
}

static void resolve_mode_flags(const struct bridge_config *cfg, bool *out_force_auto, bool *out_disable_auto)
{
    bool force_auto = cfg->has_force_auto ? cfg->force_auto : false;
    bool disable_auto = cfg->has_disable_auto ? cfg->disable_auto : false;

    const char *env_mode = getenv("DISCORD_RPC_MODE");
    if (env_mode && *env_mode) {
        if (strcmp(env_mode, "fallback") == 0 || strcmp(env_mode, "instant") == 0) {
            force_auto = true;
            disable_auto = false;
        } else if (strcmp(env_mode, "native_only") == 0) {
            disable_auto = true;
            force_auto = false;
        } else if (strcmp(env_mode, "auto") == 0) {
            force_auto = false;
            disable_auto = false;
        }
    }

    const char *env_force = getenv("NOTPROTON_DISCORD_FORCE_AUTO");
    if (env_force) force_auto = (strcmp(env_force, "0") != 0);
    const char *env_no_auto = getenv("NOTPROTON_DISCORD_DISABLE_AUTO");
    if (env_no_auto) disable_auto = (strcmp(env_no_auto, "0") != 0);

    *out_force_auto = force_auto;
    *out_disable_auto = disable_auto;
}

struct auto_presence_args {
    char appid[64];
    char game_name[256];
    time_t start_time;
    struct bridge_config cfg;
};

static bool is_ipc_error(const char *resp)
{
    return strstr(resp, "\"evt\":\"ERROR\"") != NULL ||
           strstr(resp, "\"evt\": \"ERROR\"") != NULL;
}

static void *auto_presence_thread(void *arg)
{
    struct auto_presence_args *info = (struct auto_presence_args *)arg;

    bool force_auto = false;
    bool disable_auto = false;
    resolve_mode_flags(&info->cfg, &force_auto, &disable_auto);
    if (disable_auto) {
        free(info);
        return NULL;
    }

    const char *custom_title = getenv_alias("DISCORD_RPC_GAME_NAME", "NOTPROTON_DISCORD_TITLE");
    if (custom_title && *custom_title) {
        strncpy(info->game_name, custom_title, sizeof(info->game_name) - 1);
        info->game_name[sizeof(info->game_name) - 1] = '\0';
    }

    log_bridge("[Auto-Presence] Started for game: \"%s\" (AppID: %s)", info->game_name, info->appid[0] ? info->appid : "none");

    int wait_seconds = info->cfg.has_wait_sec ? info->cfg.wait_sec : 3;
    const char *env_wait = getenv_alias("DISCORD_RPC_FALLBACK_DELAY", "NOTPROTON_DISCORD_WAIT_SEC");
    if (env_wait && *env_wait) {
        int w = atoi(env_wait);
        if (w >= 0 && w <= 60) wait_seconds = w;
    }

    // Grace period to see if the game connects with native RPC
    if (!force_auto && wait_seconds > 0) {
        int steps = wait_seconds * 10;
        for (int i = 0; i < steps; ++i) {
            if (g_stopping || g_game_rpc_active) {
                free(info);
                return NULL;
            }
            struct timespec delay = {0, 100000000}; // 100ms
            nanosleep(&delay, NULL);
        }
    }

    if (g_game_rpc_active || g_stopping) {
        free(info);
        return NULL;
    }

    int discord_fd = -1;
    char safe_title[256];
    json_escape(info->game_name, safe_title, sizeof(safe_title));

    char safe_state[256];
    const char *custom_state = getenv_alias("DISCORD_RPC_SUBTITLE", "NOTPROTON_DISCORD_STATE");
    if (custom_state) {
        json_escape(custom_state, safe_state, sizeof(safe_state));
    } else if (info->cfg.has_state) {
        json_escape(info->cfg.state, safe_state, sizeof(safe_state));
    } else {
        snprintf(safe_state, sizeof(safe_state), "Steam via NotProton");
    }

    const char *art_pref = getenv_alias("DISCORD_RPC_ARTWORK", "NOTPROTON_DISCORD_ART");
    if ((!art_pref || !*art_pref) && info->cfg.has_art) {
        art_pref = info->cfg.art;
    }

    char safe_small_image[768] = {0};
    const char *env_small = getenv("DISCORD_RPC_SMALL_IMAGE");
    if (env_small) {
        if (*env_small) json_escape(env_small, safe_small_image, sizeof(safe_small_image));
    } else if (info->cfg.has_small_image && info->cfg.small_image[0]) {
        json_escape(info->cfg.small_image, safe_small_image, sizeof(safe_small_image));
    }

    time_t last_broadcast = 0;
    bool use_details_fallback = false;

    while (!g_stopping && !g_game_rpc_active) {
        time_t current_time = time(NULL);

        if (discord_fd < 0) {
            discord_fd = connect_discord();
            if (discord_fd >= 0) {
                const char *client_id = DEFAULT_DISCORD_CLIENT_ID;
                const char *env_cid = getenv("DISCORD_RPC_CLIENT_ID");
                if (env_cid && *env_cid) client_id = env_cid;
                else if (info->cfg.has_client_id && info->cfg.client_id[0]) client_id = info->cfg.client_id;

                char handshake[256];
                snprintf(handshake, sizeof(handshake), "{\"v\":1,\"client_id\":\"%s\"}", client_id);
                if (write_ipc_frame(discord_fd, 0, handshake) == 0) {
                    uint32_t op = 0;
                    char resp[4096];
                    if (read_ipc_frame(discord_fd, &op, resp, sizeof(resp)) != 0 || op == 2) {
                        close(discord_fd);
                        discord_fd = -1;
                    } else {
                        last_broadcast = 0;
                    }
                } else {
                    close(discord_fd);
                    discord_fd = -1;
                }
            }
        }

        if (discord_fd >= 0 && (last_broadcast == 0 || (current_time - last_broadcast >= 30))) {
            char image_url[512] = {0};
            if (info->appid[0]) {
                if (art_pref && strcmp(art_pref, "icon") == 0) {
                    char icon_hash[64];
                    if (get_game_icon_hash(info->appid, icon_hash, sizeof(icon_hash))) {
                        snprintf(image_url, sizeof(image_url),
                                 "https://cdn.cloudflare.steamstatic.com/steamcommunity/public/images/apps/%s/%s.jpg",
                                 info->appid, icon_hash);
                    } else {
                        snprintf(image_url, sizeof(image_url),
                                 "https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/%s/library_600x900.jpg",
                                 info->appid);
                    }
                } else if (art_pref && strcmp(art_pref, "header") == 0) {
                    snprintf(image_url, sizeof(image_url),
                             "https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/%s/header.jpg",
                             info->appid);
                } else if (art_pref && strcmp(art_pref, "hero") == 0) {
                    snprintf(image_url, sizeof(image_url),
                             "https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/%s/library_hero.jpg",
                             info->appid);
                } else if (art_pref && strcmp(art_pref, "logo") == 0) {
                    snprintf(image_url, sizeof(image_url),
                             "https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/%s/logo.png",
                             info->appid);
                } else {
                    // Default: Official high-definition vertical cover art (600x900)
                    snprintf(image_url, sizeof(image_url),
                             "https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/%s/library_600x900.jpg",
                             info->appid);
                }
            }

            char state_field[300];
            if (safe_state[0]) {
                snprintf(state_field, sizeof(state_field), "\"state\":\"%s\",", safe_state);
            } else {
                state_field[0] = '\0';
            }

            char assets_field[1600];
            if (image_url[0] && safe_small_image[0]) {
                snprintf(assets_field, sizeof(assets_field),
                         ",\"assets\":{\"large_image\":\"%s\",\"large_text\":\"%s\",\"small_image\":\"%s\"}",
                         image_url, safe_title, safe_small_image);
            } else if (image_url[0]) {
                snprintf(assets_field, sizeof(assets_field),
                         ",\"assets\":{\"large_image\":\"%s\",\"large_text\":\"%s\"}",
                         image_url, safe_title);
            } else if (safe_small_image[0]) {
                snprintf(assets_field, sizeof(assets_field),
                         ",\"assets\":{\"small_image\":\"%s\"}",
                         safe_small_image);
            } else {
                assets_field[0] = '\0';
            }

            const char *nonce_id = info->appid[0] ? info->appid : "game";
            char json[4096];
            if (!use_details_fallback) {
                snprintf(json, sizeof(json),
                         "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%ld,\"activity\":{"
                         "\"name\":\"%s\","
                         "\"type\":0,"
                         "\"status_display_type\":2,"
                         "%s"
                         "\"timestamps\":{\"start\":%lld}"
                         "%s"
                         "}},\"nonce\":\"notproton-%s-%ld\"}",
                         (long)getpid(), safe_title, state_field,
                         (long long)info->start_time, assets_field, nonce_id, (long)current_time);
            } else {
                snprintf(json, sizeof(json),
                         "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%ld,\"activity\":{"
                         "\"type\":0,"
                         "\"details\":\"Playing %s\","
                         "%s"
                         "\"timestamps\":{\"start\":%lld}"
                         "%s"
                         "}},\"nonce\":\"notproton-%s-%ld\"}",
                         (long)getpid(), safe_title, state_field,
                         (long long)info->start_time, assets_field, nonce_id, (long)current_time);
            }

            if (write_ipc_frame(discord_fd, 1, json) == 0) {
                uint32_t op = 0;
                char resp[4096];
                if (read_ipc_frame(discord_fd, &op, resp, sizeof(resp)) == 0 && op != 2) {
                    if (!use_details_fallback && is_ipc_error(resp)) {
                        log_bridge("[Auto-Presence] Discord rejected name override; retrying with details fallback");
                        use_details_fallback = true;
                        snprintf(json, sizeof(json),
                                 "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%ld,\"activity\":{"
                                 "\"type\":0,"
                                 "\"details\":\"Playing %s\","
                                 "%s"
                                 "\"timestamps\":{\"start\":%lld}"
                                 "%s"
                                 "}},\"nonce\":\"notproton-%s-%ld\"}",
                                 (long)getpid(), safe_title, state_field,
                                 (long long)info->start_time, assets_field, nonce_id, (long)current_time);
                        if (write_ipc_frame(discord_fd, 1, json) == 0 &&
                            read_ipc_frame(discord_fd, &op, resp, sizeof(resp)) == 0 &&
                            op != 2 && !is_ipc_error(resp)) {
                            if (last_broadcast == 0) {
                                log_bridge("[Auto-Presence] Broadcasted Discord presence (details fallback): \"Playing %s\"", info->game_name);
                            }
                            last_broadcast = current_time;
                        } else {
                            close(discord_fd);
                            discord_fd = -1;
                        }
                    } else {
                        if (last_broadcast == 0) {
                            log_bridge("[Auto-Presence] Broadcasted Discord presence: \"Playing %s\" with cover art and timer", info->game_name);
                        }
                        last_broadcast = current_time;
                    }
                } else {
                    close(discord_fd);
                    discord_fd = -1;
                }
            } else {
                close(discord_fd);
                discord_fd = -1;
            }
        }

        for (int i = 0; i < 10; ++i) {
            if (g_stopping || g_game_rpc_active) break;
            struct timespec delay = {0, 100000000};
            nanosleep(&delay, NULL);
        }
    }

    if (discord_fd >= 0) {
        char clear_json[256];
        snprintf(clear_json, sizeof(clear_json),
                 "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%ld,\"activity\":null},"
                 "\"nonce\":\"notproton-clear-%ld\"}",
                 (long)getpid(), (long)time(NULL));
        write_ipc_frame(discord_fd, 1, clear_json);
        uint32_t op = 0;
        char resp[1024];
        read_ipc_frame(discord_fd, &op, resp, sizeof(resp));
        close(discord_fd);
        if (g_game_rpc_active) {
            log_bridge("[Auto-Presence] Native game RPC initiated. Auto-presence cleared and stopped.");
        } else {
            log_bridge("[Auto-Presence] Game exited. Discord activity cleared.");
        }
    }

    free(info);
    return NULL;
}
#endif

// -----------------------------------------------------------------------------
// STANDALONE CLI MODE
// -----------------------------------------------------------------------------
#ifdef BUILD_HOST_CLI

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: discord-host WINELOADER bridge.exe\n");
        return 64;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, stop_signal);
    signal(SIGINT, stop_signal);
    signal(SIGHUP, stop_signal);
    pid_t parent = getppid();

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t size = sizeof(address);
    if (listener < 0 || !configure_socket(listener) ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) ||
        getsockname(listener, (struct sockaddr *)&address, &size) ||
        listen(listener, BRIDGE_CLIENTS)) {
        perror("NotProton Discord: listen");
        if (listener >= 0) close(listener);
        return 1;
    }

    unsigned char random[BRIDGE_TOKEN_LENGTH / 2];
    char token[BRIDGE_TOKEN_LENGTH + 1], port[16];
    arc4random_buf(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); ++i) snprintf(token + i * 2, 3, "%02x", random[i]);
    snprintf(port, sizeof(port), "%u", ntohs(address.sin_port));

    if (setenv("NOTPROTON_DISCORD_PORT", port, 1) || setenv("NOTPROTON_DISCORD_TOKEN", token, 1)) {
        perror("NotProton Discord: helper environment");
        close(listener);
        return 1;
    }

    // The IPC helper must not initialize Steam's overlay or claim a physical controller.
    unsetenv("DYLD_INSERT_LIBRARIES");
    unsetenv("STEAM_DYLD_INSERT_LIBRARIES");
    unsetenv("NOTPROTON_GAME_LOADER");

    char *arguments[] = {argv[1], argv[2], NULL};
    pid_t child;
    int error = posix_spawn(&child, argv[1], NULL, NULL, arguments, environ);
    unsetenv("NOTPROTON_DISCORD_PORT");
    unsetenv("NOTPROTON_DISCORD_TOKEN");
    if (error) {
        fprintf(stderr, "NotProton Discord: %s\n", strerror(error));
        close(listener);
        return 1;
    }

    int status = run_bridge_loop(listener, token, child, -1, parent);
    return status;
}

// -----------------------------------------------------------------------------
// INJECTED DYLIB MODE (NotProton Plugin Architecture)
// -----------------------------------------------------------------------------
#else

static pthread_t g_thread;
static int g_wakeup_pipe[2] = {-1, -1};
static bool g_initialized = false;

struct worker_params {
    char wine_path[1024];
    char helper_path[1024];
};

static void *bridge_thread_proc(void *arg)
{
    struct worker_params *params = (struct worker_params *)arg;

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t size = sizeof(address);
    if (listener < 0 || !configure_socket(listener) ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) ||
        getsockname(listener, (struct sockaddr *)&address, &size) ||
        listen(listener, BRIDGE_CLIENTS)) {
        if (listener >= 0) close(listener);
        free(params);
        return NULL;
    }

    unsigned char random[BRIDGE_TOKEN_LENGTH / 2];
    char token[BRIDGE_TOKEN_LENGTH + 1], port_str[16];
    arc4random_buf(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); ++i) snprintf(token + i * 2, 3, "%02x", random[i]);
    snprintf(port_str, sizeof(port_str), "%u", ntohs(address.sin_port));

    // Build custom environment for posix_spawn
    // Strip DYLD_INSERT_LIBRARIES so helper doesn't re-inject us
    size_t env_count = 0;
    while (environ && environ[env_count]) ++env_count;

    char **child_env = calloc(env_count + 8, sizeof(char *));
    size_t out_idx = 0;
    for (size_t i = 0; i < env_count; ++i) {
        const char *var = environ[i];
        if (strncmp(var, "DYLD_INSERT_LIBRARIES=", 22) == 0 ||
            strncmp(var, "STEAM_DYLD_INSERT_LIBRARIES=", 28) == 0 ||
            strncmp(var, "NOTPROTON_GAME_LOADER=", 22) == 0 ||
            strncmp(var, "NOTPROTON_DISCORD_PORT=", 23) == 0 ||
            strncmp(var, "NOTPROTON_DISCORD_TOKEN=", 24) == 0) {
            continue;
        }
        child_env[out_idx++] = strdup(var);
    }

    char port_buf[64], token_buf[128];
    snprintf(port_buf, sizeof(port_buf), "NOTPROTON_DISCORD_PORT=%s", port_str);
    snprintf(token_buf, sizeof(token_buf), "NOTPROTON_DISCORD_TOKEN=%s", token);
    child_env[out_idx++] = strdup(port_buf);
    child_env[out_idx++] = strdup(token_buf);
    child_env[out_idx++] = strdup("NOTPROTON_DISCORD_ACTIVE=1");
    child_env[out_idx] = NULL;

    char *arguments[] = {params->wine_path, params->helper_path, NULL};
    pid_t child = -1;
    log_bridge("Spawning helper: wine=%s helper=%s", params->wine_path, params->helper_path);
    int error = posix_spawn(&child, params->wine_path, NULL, NULL, arguments, child_env);

    for (size_t i = 0; i < out_idx; ++i) free(child_env[i]);
    free(child_env);

    if (error == 0) {
        log_bridge("Helper spawned successfully (pid=%d), running bridge event loop", (int)child);
        run_bridge_loop(listener, token, child, g_wakeup_pipe[0], 0);
        g_stopping = 1;
    } else {
        log_bridge("Failed to spawn helper: %s (error %d)", strerror(error), error);
        close(listener);
    }

    free(params);
    return NULL;
}

__attribute__((constructor))
static void discord_bridge_init(void)
{
    const char *env_disable = getenv_alias("DISCORD_RPC_DISABLE", "NOTPROTON_DISCORD_DISABLE");
    if (env_disable && strcmp(env_disable, "0") != 0) return;

    // Prevent double-init or recursing into child processes
    if (getenv("NOTPROTON_DISCORD_ACTIVE")) return;
    setenv("NOTPROTON_DISCORD_ACTIVE", "1", 1);
    log_bridge("discord_bridge_init: Injected into pid=%d", getpid());

    // Find the wine executable path
    char wine_path[1024] = {0};
    const char *env_wineloader = getenv("WINELOADER");
    if (env_wineloader && *env_wineloader && access(env_wineloader, X_OK) == 0) {
        strncpy(wine_path, env_wineloader, sizeof(wine_path) - 1);
    } else {
        uint32_t buf_size = sizeof(wine_path);
        if (_NSGetExecutablePath(wine_path, &buf_size) != 0) return;
    }

    // Only activate if running inside a Wine process
    if (strstr(wine_path, "wine") == NULL) return;
    Dl_info info;
    if (!dladdr((void *)&discord_bridge_init, &info) || !info.dli_fname) return;

    char plugin_dir[1024];
    strncpy(plugin_dir, info.dli_fname, sizeof(plugin_dir) - 1);
    plugin_dir[sizeof(plugin_dir) - 1] = '\0';
    char *slash = strrchr(plugin_dir, '/');
    if (!slash) return;
    *slash = '\0';

    char helper_path[1024];
    snprintf(helper_path, sizeof(helper_path), "%s/notproton-discord.exe", plugin_dir);

    if (access(helper_path, R_OK) != 0) {
        // Helper executable not present
        return;
    }

    struct bridge_config cfg;
    load_bridge_config(plugin_dir, &cfg);
    if (cfg.has_socket_dir && !getenv("DISCORD_RPC_SOCKET_DIR") && !getenv("NOTPROTON_DISCORD_SOCKET_DIR")) {
        setenv("DISCORD_RPC_SOCKET_DIR", cfg.socket_dir, 0);
    }

    bool force_auto = false;
    bool disable_auto = false;
    resolve_mode_flags(&cfg, &force_auto, &disable_auto);
    (void)disable_auto;

    // Detect Steam Game (or install folder / custom game_name) and spawn Auto-Presence
    char appid[64] = {0};
    extract_appid(wine_path, appid, sizeof(appid));
    char game_name[256] = {0};
    const char *custom_title = getenv_alias("DISCORD_RPC_GAME_NAME", "NOTPROTON_DISCORD_TITLE");
    bool has_name = false;
    if (custom_title && *custom_title) {
        strncpy(game_name, custom_title, sizeof(game_name) - 1);
        game_name[sizeof(game_name) - 1] = '\0';
        has_name = true;
    } else {
        has_name = get_game_name_for_appid(appid, game_name, sizeof(game_name));
    }

    if (has_name) {
        struct auto_presence_args *args = malloc(sizeof(*args));
        if (args) {
            memset(args, 0, sizeof(*args));
            strncpy(args->appid, appid, sizeof(args->appid) - 1);
            strncpy(args->game_name, game_name, sizeof(args->game_name) - 1);
            args->start_time = time(NULL);
            args->cfg = cfg;
            pthread_t auto_thread;
            pthread_attr_t auto_attr;
            pthread_attr_init(&auto_attr);
            pthread_attr_setstacksize(&auto_attr, 1024 * 1024);
            pthread_create(&auto_thread, &auto_attr, auto_presence_thread, args);
            pthread_detach(auto_thread);
            pthread_attr_destroy(&auto_attr);
        }
    }

    if (force_auto) {
        log_bridge("mode=fallback enabled: running pure Auto-Presence mode (skipping Windows pipe helper)");
        return;
    }

    if (pipe(g_wakeup_pipe) < 0) return;
    configure_socket(g_wakeup_pipe[0]);
    configure_socket(g_wakeup_pipe[1]);

    struct worker_params *params = malloc(sizeof(*params));
    if (!params) return;
    strncpy(params->wine_path, wine_path, sizeof(params->wine_path) - 1);
    strncpy(params->helper_path, helper_path, sizeof(params->helper_path) - 1);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);

    g_initialized = true;
    if (pthread_create(&g_thread, &attr, bridge_thread_proc, params) != 0) {
        free(params);
        g_initialized = false;
        close(g_wakeup_pipe[0]);
        close(g_wakeup_pipe[1]);
        g_wakeup_pipe[0] = g_wakeup_pipe[1] = -1;
    }
    pthread_attr_destroy(&attr);
}

__attribute__((destructor))
static void discord_bridge_fini(void)
{
    if (!g_initialized) return;
    g_stopping = 1;
    if (g_wakeup_pipe[1] >= 0) {
        char wake = 1;
        (void)write(g_wakeup_pipe[1], &wake, 1);
    }
    pthread_join(g_thread, NULL);
    if (g_wakeup_pipe[0] >= 0) close(g_wakeup_pipe[0]);
    if (g_wakeup_pipe[1] >= 0) close(g_wakeup_pipe[1]);
    g_wakeup_pipe[0] = g_wakeup_pipe[1] = -1;
    g_initialized = false;
}

#endif
