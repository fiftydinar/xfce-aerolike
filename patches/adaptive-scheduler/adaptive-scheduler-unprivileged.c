/*
 * adaptive-scheduler-unprivileged: per-user helper for adaptive-scheduler.
 *
 * The root daemon (adaptive-scheduler) deliberately runs with only
 * CAP_SYS_NICE: it must NOT read user process memory (that would need
 * CAP_SYS_PTRACE) or write user-owned cgroup files / the user's XAUTHORITY
 * (that would need CAP_DAC_OVERRIDE). Two memory operations can therefore
 * only happen inside the user's own domain, and this helper performs them:
 *
 *   1. Writing the user's own memory.low files. systemd --user chowns the
 *      delegated cgroup files to the user, so root without CAP_DAC_OVERRIDE
 *      gets EACCES -- but the user himself can write them.
 *   2. Reading /proc/<pid>/maps of the user's own processes to collect
 *      address ranges. Cross-user maps reads require CAP_SYS_PTRACE; the
 *      user can always read his own.
 *
 * It also keeps reporting the active X window PID (the job of the old
 * aerolike-fg script this helper replaces): the daemon cannot reach the
 * user's X server (no XAUTHORITY), so the report must come from inside
 * the graphical session.
 *
 * Runs as a systemd user unit inside the graphical session, where DISPLAY
 * and XAUTHORITY are set. No special privileges; a plain per-user process.
 *
 * IPC via /tmp (files are world-readable/writable; content is validated):
 *   - /tmp/aerolike-fg-<uid>     helper -> daemon: active X window PID
 *   - /tmp/aerolike-tier-<uid>   daemon -> helper: <scope> <memory.low bytes>
 *   - /tmp/aerolike-trim-<uid>   daemon -> helper: pids to trim
 *   - /tmp/aerolike-ranges-<uid> helper -> daemon: <pid> <hex ranges>
 *
 * Security: every path/pid taken from the daemon-provided files is
 * re-checked against the helper's own uid (cgroup paths must live inside
 * this user's own tree; pids must have this user's euid), so a forged
 * /tmp file cannot redirect the work at another user's cgroups or
 * processes.
 *
 * Logs state transitions and errors to stderr (journald):
 *   journalctl --user -u adaptive-scheduler-unprivileged
 *   (startup, active-window changes, memory.low changes, trim-cycle
 *   starts, and rate-limited errors; no per-second noise).
 *
 * Build: gcc -O2 -o adaptive-scheduler-unprivileged adaptive-scheduler-unprivileged.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <stdbool.h>
#include <stdarg.h>

static char uidbuf[32];

/* ---------- logging ---------- */

/* Runs as a systemd user unit, so stderr is captured by journald (visible
 * with: journalctl --user -u adaptive-scheduler-unprivileged). Log state
 * transitions and errors only -- the 1s loop must not spam the journal. */
static void log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "adaptive-scheduler-unprivileged: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static void log_ratelimited(time_t *last, int min_interval, const char *fmt, ...) {
    time_t now = time(NULL);
    if (*last && now - *last < min_interval) return;
    *last = now;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "adaptive-scheduler-unprivileged: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* Last path component of a scope, for readable log lines. */
static const char *scope_name(const char *path) {
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

static unsigned long long total_ram_bytes(void) {
    long pages = sysconf(_SC_PHYS_PAGES);
    long psz = sysconf(_SC_PAGESIZE);
    if (pages <= 0 || psz <= 0) return 8ULL * 1024 * 1024 * 1024; /* 8G fallback */
    return (unsigned long long)pages * (unsigned long long)psz;
}

/* A forged /tmp file could name any pid; only accept pids that really
 * belong to this user (euid == getuid()). /proc/<pid>/status is
 * world-readable, so the check works for every candidate pid. */
static bool pid_belongs_to_me(pid_t pid) {
    char path[64];
    char line[256];
    snprintf(path, sizeof path, "/proc/%ld/status", (long)pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return false;
    bool match = false;
    while (fgets(line, sizeof line, fp)) {
        if (strncmp(line, "Uid:", 4) == 0) {
            unsigned long ruid, euid;
            if (sscanf(line + 4, "%lu %lu", &ruid, &euid) == 2)
                match = (euid == (unsigned long)getuid());
            break;
        }
    }
    fclose(fp);
    return match;
}

/* Write a /tmp file atomically (tmp file + rename) so a concurrent reader
 * (the root daemon, or us) never sees a torn write. */
static void atomic_write(const char *path, const char *data) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    size_t len = strlen(data);
    ssize_t w = len ? write(fd, data, len) : 0;
    close(fd);
    if (w < 0 || (size_t)w != len) {
        unlink(tmp);
        return;
    }
    if (rename(tmp, path) != 0) unlink(tmp);
}

/* ---------- active X window PID ---------- */

static void report_fg(void) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/aerolike-fg-%s", uidbuf);

    FILE *p = popen("xdotool getactivewindow getwindowpid 2>/dev/null", "r");
    if (!p) return;
    char buf[64];
    pid_t pid = 0;
    if (fgets(buf, sizeof buf, p))
        pid = (pid_t)strtol(buf, NULL, 10);
    pclose(p);

    if (pid > 0 && !pid_belongs_to_me(pid))
        pid = 0;

    if (pid > 0) {
        char s[32];
        snprintf(s, sizeof s, "%d\n", (int)pid);
        atomic_write(path, s);
    } else {
        /* No (valid) focused window: drop the stale pid so the daemon
         * stops treating the last foreground app as current. */
        unlink(path);
    }

    /* Log only when the active window actually changes. */
    static pid_t last_pid = -1;
    if (last_pid != pid) {
        log_info("active window pid %d%s", pid, pid ? "" : " (none focused)");
        last_pid = pid;
    }
}

/* ---------- memory.low gradation ---------- */

/* Apply the daemon's memory.low targets (from /tmp/aerolike-tier-<uid>)
 * to our own scopes. Only touch cgroup paths that are provably inside
 * this user's own tree: /sys/fs/cgroup/user.slice/user-<uid>.slice/
 * user@<uid>.service/. A local attacker's forged tier file can therefore
 * at worst steer our own memory protection. */
static void apply_tiers(void) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/aerolike-tier-%s", uidbuf);
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char prefix[PATH_MAX];
    snprintf(prefix, sizeof prefix,
             "/sys/fs/cgroup/user.slice/user-%s.slice/user@%s.service/",
             uidbuf, uidbuf);

    char line[PATH_MAX + 64];
    while (fgets(line, sizeof line, fp)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        char *scope = line;
        char *num = tab + 1;

        if (strncmp(scope, prefix, strlen(prefix)) != 0) continue;
        if (strstr(scope, "..") != NULL) continue; /* no traversal games */

        char *end = NULL;
        unsigned long long val = strtoull(num, &end, 10);
        while (*end == ' ' || *end == '\n') end++;
        if (*end != 0) continue; /* not a plain number, ignore */
        unsigned long long cap = total_ram_bytes();
        if (val > cap) val = cap;

        char mpath[PATH_MAX];
        snprintf(mpath, sizeof mpath, "%s/memory.low", scope);
        int fd = open(mpath, O_WRONLY);
        if (fd < 0) continue;
        char buf[32];
        int n = snprintf(buf, sizeof buf, "%llu\n", val);
        ssize_t wr = (n > 0) ? write(fd, buf, (size_t)n) : -1;
        close(fd);

        /* Log only when the applied value actually changes (scopes change
         * tier rarely), plus write errors (ratelimited). */
        static time_t err_last = 0;
        if (wr < 0) {
            log_ratelimited(&err_last, 30, "memory.low write %s: %s",
                            scope_name(scope), strerror(errno));
            continue;
        }
        static struct { char scope[256]; unsigned long long val; } last[64];
        static int last_n = 0;
        const char *sname = scope_name(scope);
        int slot = -1;
        for (int i = 0; i < last_n; i++)
            if (strcmp(last[i].scope, sname) == 0) { slot = i; break; }
        if (slot < 0 && last_n < 64) {
            snprintf(last[last_n].scope, sizeof last[0].scope, "%s", sname);
            last[last_n].val = 0;
            slot = last_n++;
        }
        if (slot >= 0 && last[slot].val != val) {
            log_info("memory.low %s = %llu bytes",
                     sname, (unsigned long long)val);
            last[slot].val = val;
        }
    }
    fclose(fp);
}

/* ---------- working-set trim: address ranges ---------- */

/* For each pid the daemon asks us to trim (from /tmp/aerolike-trim-<uid>),
 * read our own /proc/<pid>/maps and emit the mapping ranges to
 * /tmp/aerolike-ranges-<uid>, one "<pid>\t<start>-<end>,<start>-<end>…"
 * line per pid. The daemon feeds those ranges to process_madvise(MADV_COLD),
 * which the kernel gates on CAP_SYS_NICE (we don't have it). Written
 * atomically so the daemon never sees a partial file. */
static void collect_ranges(void) {
    char tpath[64], rpath[64], rtmp[64];
    snprintf(tpath, sizeof tpath, "/tmp/aerolike-trim-%s", uidbuf);
    snprintf(rpath, sizeof rpath, "/tmp/aerolike-ranges-%s", uidbuf);
    snprintf(rtmp, sizeof rtmp, "/tmp/aerolike-ranges-%s.tmp", uidbuf);

    FILE *fp = fopen(tpath, "r");
    if (!fp) return;
    FILE *out = fopen(rtmp, "w");
    if (!out) {
        fclose(fp);
        return;
    }

    char line[256];
    static time_t maps_err_last = 0;
    int produced = 0;
    while (fgets(line, sizeof line, fp)) {
        pid_t pid = (pid_t)strtol(line, NULL, 10);
        if (pid <= 0 || !pid_belongs_to_me(pid)) continue;

        char mpath[64];
        snprintf(mpath, sizeof mpath, "/proc/%ld/maps", (long)pid);
        FILE *m = fopen(mpath, "r");
        if (!m) {
            log_ratelimited(&maps_err_last, 30,
                            "trim: cannot read maps of pid %ld: %s",
                            (long)pid, strerror(errno));
            continue;
        }

        fprintf(out, "%ld\t", (long)pid);
        char mline[4096];
        int first = 1;
        while (fgets(mline, sizeof mline, m)) {
            char *tok = strtok(mline, " \t");
            if (!tok) continue;
            /* First field is "<start>-<end>" in hex; verify before trusting. */
            char *dash = strchr(tok, '-');
            if (!dash) continue;
            char *endp = NULL;
            (void)strtoul(tok, &endp, 16);
            if (endp != dash) continue;
            unsigned long e = strtoul(dash + 1, &endp, 16);
            if (*endp != 0) continue;
            unsigned long s = strtoul(tok, NULL, 16);
            if (e <= s) continue;

            if (!first) fputc(',', out);
            first = 0;
            fputs(tok, out);
        }
        fputc('\n', out);
        fclose(m);
        produced++;
    }
    fclose(out);
    fclose(fp);
    rename(rtmp, rpath);

    /* Log only when a trim cycle begins (ranges going from empty to
     * non-empty), not on every pass -- the daemon logs the actual
     * MADV_COLD result. */
    static bool had_ranges = false;
    bool now_had = produced > 0;
    if (now_had && !had_ranges)
        log_info("trim: collecting ranges for %d pid(s)", produced);
    had_ranges = now_had;
}

/* ---------- main ---------- */

static volatile sig_atomic_t running = 1;
static void on_signal(int sig) { (void)sig; running = 0; }

int main(void) {
    snprintf(uidbuf, sizeof uidbuf, "%lu", (unsigned long)getuid());

    log_info("started: uid %s, DISPLAY=%s, XAUTHORITY=%s",
             uidbuf, getenv("DISPLAY") ? getenv("DISPLAY") : "(none)",
             getenv("XAUTHORITY") ? getenv("XAUTHORITY") : "(none)");

    /* Clear stale ranges left by a previous helper/daemon cycle. */
    char rpath[64];
    snprintf(rpath, sizeof rpath, "/tmp/aerolike-ranges-%s", uidbuf);
    unlink(rpath);

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    while (running) {
        report_fg();
        apply_tiers();
        collect_ranges();
        sleep(1);
    }
    log_info("stopped");
    return 0;
}
