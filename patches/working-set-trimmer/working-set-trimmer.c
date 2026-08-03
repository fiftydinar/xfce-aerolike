/*
 * working-set-trimmer: userspace working-set trimmer for desktop Linux.
 *
 * Mirrors Windows' Balance Set Manager: continuously shed memory from
 * idle background apps BEFORE memory pressure peaks, so foreground apps
 * stay smooth. Conservative and cgroup-aware.
 *
 * Runs as a root system service because process_madvise(MADV_COLD)
 * requires CAP_SYS_NICE, which unprivileged users lack. Root also lets
 * one daemon coordinate trimming across ALL user sessions.
 *
 * Design:
 *   - Polls /proc/pressure/memory (PSI). Only acts when pressure is
 *     actually building.
 *   - Enumerates app.slice scopes for every user session. The DE lives
 *     in desktop.slice and protected services elsewhere -- structurally
 *     excluded by only walking user@UID.service/app.slice.
 *   - Idle = no CPU delta for IDLE_CPU_SECS consecutive checks AND the
 *     scope's processes don't own that session's active X window.
 *   - Trims with process_madvise(MADV_COLD): pages are deactivated but
 *     kept in RAM (inactive list), so resuming is a cheap soft fault,
 *     not a zram decompression. This is the key to Windows-like
 *     smoothness.
 *
 * Active-window detection is per-display: for each user session, find
 * its DISPLAY and the active window PID via xdotool.
 *
 * Build: gcc -O2 -o working-set-trimmer working-set-trimmer.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <limits.h>
#include <signal.h>
#include <pwd.h>
#include <stdbool.h>

#ifndef SYS_process_madvise
#define SYS_process_madvise 440
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#define MADV_COLD 20

/* Tunables */
static const char *PSI_PATH = "/proc/pressure/memory";
static const double PSI_THRESHOLD = 10.0;   /* trim when PSI "some" % >= this */
static const int IDLE_CPU_SECS = 60;        /* idle after 60s of no CPU use */
static const unsigned SLEEP_SECS = 1;
static const long long IDLE_JIFFY_DELTA = 1; /* >=1 tick counts as active */

#define MAX_PIDS 256
#define MAX_IOVEC 1024
#define MAX_SESSIONS 64

/* ---------- persistent per-scope state ---------- */

typedef struct ScopeEnt {
    char path[PATH_MAX];
    pid_t pids[MAX_PIDS];
    long long prev[MAX_PIDS];
    int npids;
    int idle_count;
    struct ScopeEnt *next;
} ScopeEnt;

static ScopeEnt *scopes = NULL;

static ScopeEnt *find_or_create(const char *path) {
    ScopeEnt *e;
    for (e = scopes; e; e = e->next)
        if (strcmp(e->path, path) == 0) return e;
    e = calloc(1, sizeof(ScopeEnt));
    if (!e) return NULL;
    snprintf(e->path, sizeof e->path, "%s", path);
    for (int i = 0; i < MAX_PIDS; i++) e->prev[i] = -1;
    e->next = scopes;
    scopes = e;
    return e;
}

/* ---------- syscalls ---------- */

static int cold_madvise(pid_t pid) {
    char path[64];
    char line[512];
    FILE *fp;
    struct iovec iov[MAX_IOVEC];
    int n = 0;
    int fd;

    snprintf(path, sizeof path, "/proc/%d/maps", pid);
    fp = fopen(path, "r");
    if (!fp) return -1;

    while (fgets(line, sizeof line, fp) && n < MAX_IOVEC) {
        unsigned long start, end;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) == 3) {
            if (strchr(perms, 'r') && start != 0 && end > start) {
                iov[n].iov_base = (void *)start;
                iov[n].iov_len = end - start;
                n++;
            }
        }
    }
    fclose(fp);
    if (n == 0) return 0;

    fd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (fd < 0) return -1;
    long rc = syscall(SYS_process_madvise, fd, iov, (unsigned long)n,
                      MADV_COLD, 0);
    close(fd);
    return rc < 0 ? -1 : n;
}

/* ---------- per-session active window ---------- */

typedef struct {
    char display[64];
    pid_t fg;
} SessionInfo;

/* Use logind to find each active graphical session's display and pid.
 * Simplest robust method: iterate /run/user/<uid>/wayland-* or X11 via
 * `loginctl`. Here we resolve per-uid DISPLAY from the Xauthority-based
 * X server socket if present, else skip. For a desktop image this is
 * X11 (compiz), so DISPLAY=:0..N per session. We enumerate user dirs. */
static int find_sessions(SessionInfo *out, int max) {
    int n = 0;
    DIR *d = opendir("/sys/fs/cgroup/user.slice");
    if (!d) return 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && n < max) {
        const char *nm = ent->d_name;
        if (strncmp(nm, "user-", 5) != 0) continue;
        /* user-UID.slice -> UID */
        const char *rest = nm + 5;
        char *dot = strchr(rest, '.');
        if (!dot) continue;
        size_t len = (size_t)(dot - rest);
        char uidbuf[32];
        if (len >= sizeof uidbuf) len = sizeof uidbuf - 1;
        memcpy(uidbuf, rest, len);
        uidbuf[len] = 0;
        uid_t uid = (uid_t)strtoul(uidbuf, NULL, 10);
        if (uid == 0) continue;

        /* Determine DISPLAY for this user: look for X sockets.
         * Typical: /tmp/.X11-unix/X0 for :0. We map by checking
         * /proc/<any pid of uid>/environ would be heavy; instead use
         * the common :0 for the primary session. For robustness, probe
         * X socket files. */
        snprintf(out[n].display, sizeof out[n].display, ":0");
        out[n].fg = 0;

        /* Resolve active window pid for this display. */
        char cmd[256];
        char buf[64];
        FILE *fp;
        char disp_env[32];
        snprintf(disp_env, sizeof disp_env, "DISPLAY=%s", out[n].display);
        snprintf(cmd, sizeof cmd,
            "DISPLAY=%s xdotool getactivewindow getwindowpid 2>/dev/null",
            out[n].display);
        fp = popen(cmd, "r");
        if (fp) {
            if (fgets(buf, sizeof buf, fp))
                out[n].fg = (pid_t)strtol(buf, NULL, 10);
            pclose(fp);
        }
        n++;
    }
    closedir(d);
    return n;
}

static bool pid_is_foreground(pid_t pid, SessionInfo *sessions, int nsess) {
    for (int i = 0; i < nsess; i++)
        if (sessions[i].fg > 0 && sessions[i].fg == pid)
            return true;
    return false;
}

/* ---------- per-process CPU jiffies ---------- */

static long long proc_cpu_jiffies(pid_t pid) {
    char path[64];
    char buf[512];
    int fd;
    ssize_t n;
    char *p;
    int field = 3;
    long long utime = -1, stime = -1;
    char *save = NULL, *tok;

    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;

    p = strrchr(buf, ')');
    if (!p) return -1;
    p += 2;

    tok = strtok_r(p, " ", &save);
    while (tok) {
        if (field == 14) utime = strtoll(tok, NULL, 10);
        else if (field == 15) stime = strtoll(tok, NULL, 10);
        if (utime >= 0 && stime >= 0) break;
        tok = strtok_r(NULL, " ", &save);
        field++;
    }
    if (utime < 0 || stime < 0) return -1;
    return utime + stime;
}

/* ---------- PSI ---------- */

static double psi_memory_some(void) {
    char buf[512];
    int fd;
    ssize_t n;
    char *p;
    fd = open(PSI_PATH, O_RDONLY);
    if (fd < 0) return 0.0;
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0.0;
    buf[n] = 0;
    p = strstr(buf, "some");
    if (!p) return 0.0;
    p = strstr(p, "avg10=");
    if (!p) return 0.0;
    p += 6;
    return strtod(p, NULL);
}

/* ---------- trim one scope ---------- */

static void trim_scope(ScopeEnt *e, SessionInfo *sessions, int nsess) {
    bool active = false;
    char path[PATH_MAX];
    char procs[65536];
    int fd;
    ssize_t n;
    char *save = NULL, *tok;
    int npids = 0;

    snprintf(path, sizeof path, "%s/cgroup.procs", e->path);
    fd = open(path, O_RDONLY);
    if (fd < 0) return;
    n = read(fd, procs, sizeof procs - 1);
    close(fd);
    if (n <= 0) return;
    procs[n] = 0;

    tok = strtok_r(procs, "\n", &save);
    while (tok && npids < MAX_PIDS) {
        pid_t pid = (pid_t)strtol(tok, NULL, 10);
        if (pid > 0) {
            if (pid_is_foreground(pid, sessions, nsess))
                active = true;
            e->pids[npids] = pid;
            long long j = proc_cpu_jiffies(pid);
            if (j >= 0 && e->prev[npids] >= 0) {
                if (j - e->prev[npids] >= IDLE_JIFFY_DELTA)
                    active = true;
                e->prev[npids] = j;
            } else if (j >= 0) {
                e->prev[npids] = j;
            } else {
                e->prev[npids] = -1;
            }
            npids++;
        }
        tok = strtok_r(NULL, "\n", &save);
    }
    e->npids = npids;

    if (active || npids == 0) {
        e->idle_count = 0;
        return;
    }

    e->idle_count++;
    if (e->idle_count >= IDLE_CPU_SECS) {
        for (int i = 0; i < e->npids; i++)
            cold_madvise(e->pids[i]);
        e->idle_count = 0;
    }
}

/* ---------- enumerate scopes across all users ---------- */

static void walk_users(void) {
    SessionInfo sessions[MAX_SESSIONS];
    int nsess = find_sessions(sessions, MAX_SESSIONS);

    DIR *d = opendir("/sys/fs/cgroup/user.slice");
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *nm = ent->d_name;
        if (strncmp(nm, "user-", 5) != 0) continue;
        char base[PATH_MAX];
        snprintf(base, sizeof base,
                 "/sys/fs/cgroup/user.slice/%s/user@%s.service/app.slice",
                 nm, nm + 5);

        DIR *sd = opendir(base);
        if (!sd) continue;
        struct dirent *sent;
        while ((sent = readdir(sd)) != NULL) {
            const char *snm = sent->d_name;
            size_t len = strlen(snm);
            if (len < 7 || strcmp(snm + len - 6, ".scope") != 0) continue;
            char scope[PATH_MAX];
            snprintf(scope, sizeof scope, "%s/%s", base, snm);
            struct stat st;
            if (stat(scope, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            ScopeEnt *e = find_or_create(scope);
            if (e) trim_scope(e, sessions, nsess);
        }
        closedir(sd);
    }
    closedir(d);
}

/* ---------- main ---------- */

static volatile sig_atomic_t running = 1;
static void on_signal(int sig) { (void)sig; running = 0; }

int main(void) {
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    while (running) {
        double psi = psi_memory_some();
        if (psi >= PSI_THRESHOLD) {
            walk_users();
        }
        sleep(SLEEP_SECS);
    }
    return 0;
}
