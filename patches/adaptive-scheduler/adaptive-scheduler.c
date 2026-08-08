/*
 * adaptive-scheduler: userspace adaptive resource scheduler for desktop
 * Linux -- working-set trimming + memory priority gradation + CPU nice.
 *
 * Mirrors Windows' Balance Set Manager AND memory priority classes AND
 * process priority:
 *   - Continuously shed memory from idle background apps BEFORE memory
 *     pressure peaks (working-set trimming via MADV_COLD).
 *   - Grade each app scope's memory.low so the kernel reclaims idle apps
 *     before active ones, and foreground apps last (memory priority).
 *   - Adjust CPU nice per tier so idle apps are deprioritized and active
 *     apps (even if launched from a 'terminal' name) stay at normal
 *     priority (CPU scheduling).
 *   - Boost desktop.slice's CPU priority (nice -5) so the DE always wins
 *     scheduling to respond to input and composite frames, even under
 *     heavy app CPU load. CPU-only: the DE's memory is never touched.
 *
 * Runs as a root system service because process_madvise(MADV_COLD)
 * cross-process requires CAP_SYS_NICE. One daemon coordinates across ALL
 * sessions. The service drops to CAP_SYS_NICE + NoNewPrivileges.
 *
 * Privilege separation (security):
 *   - CAP_SYS_PTRACE and CAP_DAC_OVERRIDE are deliberately NOT granted:
 *     the daemon must not read user process memory or the user's 0600
 *     XAUTHORITY file. That keeps a compromised daemon from taking over
 *     the graphical session.
 *   - Two memory operations therefore happen in the per-user domain, via
 *     the unprivileged per-user helper `adaptive-scheduler-unprivileged`
 *     (a systemd user unit running inside the session):
 *       * writing the user's own memory.low files (systemd --user chowns
 *         them to the user; root without DAC_OVERRIDE cannot write them)
 *       * reading /proc/<pid>/maps of the user's own processes to obtain
 *         address ranges (cross-user maps reads require CAP_SYS_PTRACE)
 *   - The daemon only does what needs CAP_SYS_NICE: classification
 *     (cgroup.procs, /proc/<pid>/stat, /proc/pressure/memory are all
 *     world-readable), CPU nice, the desktop boost, and
 *     process_madvise(MADV_COLD) -- which the kernel gates on
 *     CAP_SYS_NICE for cross-process advice.
 *
 * IPC via /tmp (world-readable):
 *   - /tmp/aerolike-fg-<uid>    helper -> daemon: active X window PID
 *   - /tmp/aerolike-tier-<uid>  daemon -> helper: scope -> memory.low bytes
 *   - /tmp/aerolike-trim-<uid>  daemon -> helper: pids to trim (idle under
 *                                pressure; re-published every pass while
 *                                they qualify so the helper never misses
 *                                them; the helper reads their maps)
 *   - /tmp/aerolike-ranges-<uid> helper -> daemon: pid -> address ranges,
 *                                which the daemon feeds to process_madvise
 *                                at most once per TRIM_INTERVAL per uid,
 *                                consuming each set (read + unlink).
 *   Pids in helper-provided files are re-validated against the session
 *   uid before use, so a forged file cannot target another user.
 *
 * Logs state transitions and errors to stderr (journald):
 *   journalctl -u adaptive-scheduler.service
 *   (startup + CAP check, foreground changes, tier transitions, trim
 *   requests, MADV_COLD results, and rate-limited errors; no per-second
 *   noise).
 *
 * Build: gcc -O2 -o adaptive-scheduler adaptive-scheduler.c
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
#include <time.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdarg.h>
#include <sys/resource.h>

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
static const int LONG_IDLE_SECS = 300;      /* long-idle after 5min */
static const unsigned SLEEP_SECS = 1;
static const int TRIM_INTERVAL = 30;        /* min seconds between trims/scope */
static const long long IDLE_JIFFY_DELTA = 1; /* >=1 tick counts as active */

/* page-cluster (swap-in readahead) flip thresholds, KB and passes.
 * PAGE_CLUSTER_UP_DEBOUNCE: consecutive passes with the disk swap being
 * used before enabling readahead (3); DN: consecutive passes with it
 * drained before disabling (30s of idle at 1 pass/sec). */
static const long long PAGE_CLUSTER_USED_MIN = 1024;  /* KB: >=1MB in use counts */
static const int PAGE_CLUSTER_UP_DEBOUNCE = 3;
static const int PAGE_CLUSTER_DN_DEBOUNCE = 30;

/* last written value of vm.page-cluster (0/3); starts at 0, the value
 * the shipped sysctl.d sets at boot */
static int page_cluster = 0;
static int page_used_up = 0;   /* consecutive passes swap disk in use */
static int page_used_dn = 0;   /* consecutive passes swap disk drained */

/* memory.low gradation, as fractions of total RAM */
static const double MEMLOW_FOREGROUND = 0.20;  /* 20% - most protected */
static const double MEMLOW_ACTIVE     = 0.10;  /* 10% */
static const double MEMLOW_IDLE       = 0.03;  /*  3% */
static const double MEMLOW_LONG_IDLE  = 0.0;   /*  0% - reclaim-first */

/* CPU nice per tier (dynamic, behavioral). Foreground/active get
 * normal nice so a terminal running a heavy CLI stays responsive; only
 * genuinely idle scopes are deprioritized. Negative values would need
 * CAP_SYS_NICE (which we have), but we keep >=0 to avoid starving
 * background tasks that briefly become important. */
static const int NICE_FOREGROUND = 0;   /* normal */
static const int NICE_ACTIVE     = 0;   /* normal - has CPU, don't touch */
static const int NICE_IDLE       = 5;   /* background */
static const int NICE_LONG_IDLE  = 10;  /* very background */

/* The desktop environment (desktop.slice) gets a small CPU boost so it
 * always wins scheduling over apps: the DE must respond to input and
 * composite frames even when a heavy app is spinning. Negative nice
 * requires CAP_SYS_NICE (we have it). Never apply memory actions to the
 * DE -- only CPU priority. */
static const int NICE_DESKTOP = -5;

#define MAX_PIDS 256
#define MAX_IOVEC 1024
#define MAX_SESSIONS 64

typedef enum {
    TIER_FOREGROUND,
    TIER_ACTIVE,
    TIER_IDLE,
    TIER_LONG_IDLE,
} Tier;

/* Per-uid state for throttling process_madvise (see walk_users). */
static char uid_trim[MAX_SESSIONS][32];
static time_t uid_trim_last[MAX_SESSIONS];
static int uid_trim_n = 0;

static int uid_trim_slot(const char *uid) {
    for (int i = 0; i < uid_trim_n; i++)
        if (strcmp(uid_trim[i], uid) == 0) return i;
    if (uid_trim_n < MAX_SESSIONS) {
        snprintf(uid_trim[uid_trim_n], sizeof uid_trim[0], "%s", uid);
        uid_trim_last[uid_trim_n] = 0;
        return uid_trim_n++;
    }
    return -1;
}

/* ---------- logging ---------- */

/* The daemon runs under systemd, so stderr is captured by journald with
 * timestamps. Log state transitions and errors only -- the 1s loop must
 * not spam the journal with per-pass noise. */
static void log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "adaptive-scheduler: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* Log at most once every min_interval seconds (for conditions that would
 * otherwise repeat on every pass). */
static void log_ratelimited(time_t *last, int min_interval, const char *fmt, ...) {
    time_t now = time(NULL);
    if (*last && now - *last < min_interval) return;
    *last = now;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "adaptive-scheduler: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static const char *tier_name(Tier tier) {
    switch (tier) {
    case TIER_FOREGROUND: return "FOREGROUND";
    case TIER_ACTIVE:     return "ACTIVE";
    case TIER_IDLE:       return "IDLE";
    case TIER_LONG_IDLE:  return "LONG_IDLE";
    default:              return "?";
    }
}

/* Last path component of a scope, for readable log lines. */
static const char *scope_name(const char *path) {
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

/* ---------- persistent per-scope state ---------- */

/* Per-process CPU jiffy state, keyed by pid. cgroup.procs order is not
 * stable across passes (processes die/spawn), so jiffies must be matched
 * by pid, not by array index. */
typedef struct {
    pid_t pid;
    long long prev;   /* previous pass's total jiffies, or -1 */
} ProcJiff;

typedef struct ScopeEnt {
    char path[PATH_MAX];
    ProcJiff procs[MAX_PIDS];
    int npids;
    int idle_count;
    Tier last_tier;
    bool trim_pending;   /* currently qualifying for a working-set trim */
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
    e->last_tier = TIER_ACTIVE; /* default until first classification */
    e->next = scopes;
    scopes = e;
    return e;
}

/* Drop ScopeEnt records whose cgroup no longer exists (app closed).
 * run-*.scope names are unique per launch, so without pruning the list
 * grows unboundedly over long uptimes. */
static void prune_scopes(void) {
    ScopeEnt **pp = &scopes;
    while (*pp) {
        ScopeEnt *e = *pp;
        struct stat st;
        if (stat(e->path, &st) != 0) {
            *pp = e->next;
            free(e);
        } else {
            pp = &e->next;
        }
    }
}

/* ---------- total RAM ---------- */

static unsigned long long total_ram_bytes(void) {
    long pages = sysconf(_SC_PHYS_PAGES);
    long psz = sysconf(_SC_PAGESIZE);
    if (pages <= 0 || psz <= 0) return 8ULL * 1024 * 1024 * 1024; /* 8G fallback */
    return (unsigned long long)pages * (unsigned long long)psz;
}

/* ---------- per-session active window ---------- */

typedef struct {
    char uid[32];   /* numeric uid of the session */
    pid_t fg;       /* active X window PID, reported by per-user helper */
} SessionInfo;

/* cgroup dir name "user-1000.slice" -> uid string "1000". The user's
 * session lives under user@<uid>.service (no ".slice" suffix), so the
 * suffix must be stripped before building the service path. Returns
 * false if the name is malformed. */
static bool user_uid(const char *nm, char *out, size_t outsz) {
    const char *rest = nm + 5;
    size_t len = (size_t)strcspn(rest, ".");
    if (len == 0 || len >= outsz) return false;
    memcpy(out, rest, len);
    out[len] = 0;
    return true;
}

/* The root daemon cannot talk to a user's X server: it drops
 * CAP_SYS_PTRACE (needed to read /proc/<pid>/environ cross-user) and
 * CAP_DAC_OVERRIDE (needed to read the user's XAUTHORITY file). So a
 * per-user systemd service runs xdotool inside the session -- where
 * DISPLAY and XAUTHORITY are set -- and writes the active window PID to
 * /tmp/aerolike-fg-<uid>. Read that here; 0 if none reported yet. */

/* /tmp/aerolike-fg-<uid> is world-writable, so a local user could plant
 * a pid for another uid. Validate that the reported pid actually belongs
 * to the session uid via /proc/<pid>/status (world-readable) before
 * trusting it. */
static bool pid_belongs_to(pid_t pid, unsigned long uid) {
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
                match = (euid == uid);
            break;
        }
    }
    fclose(fp);
    return match;
}

static pid_t session_foreground_pid(const char *uid_str) {
    unsigned long uid = strtoul(uid_str, NULL, 10);
    char path[64];
    char buf[64];
    snprintf(path, sizeof path, "/tmp/aerolike-fg-%s", uid_str);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    pid_t pid = (pid_t)strtol(buf, NULL, 10);
    if (pid <= 0 || !pid_belongs_to(pid, uid)) return 0;
    return pid;
}

static int find_sessions(SessionInfo *out, int max) {
    int n = 0;
    DIR *d = opendir("/sys/fs/cgroup/user.slice");
    if (!d) return 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && n < max) {
        const char *nm = ent->d_name;
        if (strncmp(nm, "user-", 5) != 0) continue;
        char uid[32];
        if (!user_uid(nm, uid, sizeof uid)) continue;
        uid_t uidn = (uid_t)strtoul(uid, NULL, 10);
        if (uidn == 0) continue;

        out[n].fg = session_foreground_pid(uid);
        snprintf(out[n].uid, sizeof out[n].uid, "%s", uid);
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

/* ---------- memory.low target ---------- */

static unsigned long long memlow_bytes(Tier tier, unsigned long long total_ram) {
    double frac;
    switch (tier) {
    case TIER_FOREGROUND: frac = MEMLOW_FOREGROUND; break;
    case TIER_ACTIVE:     frac = MEMLOW_ACTIVE;     break;
    case TIER_IDLE:       frac = MEMLOW_IDLE;       break;
    case TIER_LONG_IDLE:  frac = MEMLOW_LONG_IDLE;  break;
    default:              return 0;
    }
    return (unsigned long long)(total_ram * frac);
}

/* ---------- IPC: publish to / read from the per-user helper ---------- */

static void write_uid_file(const char *kind, const char *uid_str,
                           const char *buf, size_t len) {
    /* Write atomically (tmp file + rename) so the per-user helper never
     * sees a torn tier/trim file while it is being replaced. */
    char path[64];
    char tmp[80];
    snprintf(path, sizeof path, "/tmp/aerolike-%s-%s", kind, uid_str);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (len && write(fd, buf, len) != (ssize_t)len) {
        close(fd);
        unlink(tmp);
        return;
    }
    close(fd);
    if (rename(tmp, path) != 0) unlink(tmp);
}

static int tier_nice(Tier tier) {
    switch (tier) {
    case TIER_FOREGROUND: return NICE_FOREGROUND;
    case TIER_ACTIVE:     return NICE_ACTIVE;
    case TIER_IDLE:       return NICE_IDLE;
    case TIER_LONG_IDLE:  return NICE_LONG_IDLE;
    default:              return 0;
    }
}

static void set_nice(pid_t pid, Tier tier) {
    errno = 0;
    int rc = setpriority(PRIO_PROCESS, pid, tier_nice(tier));
    /* On failure (ESRCH if it died) just ignore. */
    (void)rc;
}

/* ---------- classify + grade + request trim for one scope ---------- */

static void process_scope(ScopeEnt *e, SessionInfo *sessions, int nsess,
                          double psi, unsigned long long total_ram,
                          char *tier_buf, size_t tier_cap, size_t *tier_len,
                          char *trim_buf, size_t trim_cap, size_t *trim_len) {
    bool has_foreground = false;
    bool has_active = false;
    char path[PATH_MAX];
    char procs[65536];
    int fd;
    ssize_t n;
    char *save = NULL, *tok;

    snprintf(path, sizeof path, "%s/cgroup.procs", e->path);
    fd = open(path, O_RDONLY);
    if (fd < 0) return;
    n = read(fd, procs, sizeof procs - 1);
    close(fd);
    if (n <= 0) return;
    procs[n] = 0;

    tok = strtok_r(procs, "\n", &save);
    ProcJiff cur[MAX_PIDS];
    int ncur = 0;
    while (tok && ncur < MAX_PIDS) {
        pid_t pid = (pid_t)strtol(tok, NULL, 10);
        if (pid > 0) {
            if (pid_is_foreground(pid, sessions, nsess))
                has_foreground = true;
            /* Match jiffy state by pid: cgroup.procs order is not stable
             * across passes, so an index match could compare two
             * different processes (transient false ACTIVE/IDLE). Read the
             * previous pass's state into a temp buffer first so lookups
             * are not disturbed by in-place overwrite. */
            long long prev = -1;
            for (int i = 0; i < e->npids; i++)
                if (e->procs[i].pid == pid) { prev = e->procs[i].prev; break; }
            long long j = proc_cpu_jiffies(pid);
            if (j >= 0 && prev >= 0 && j - prev >= IDLE_JIFFY_DELTA)
                has_active = true;
            cur[ncur].pid = pid;
            cur[ncur].prev = (j >= 0) ? j : -1;
            ncur++;
        }
        tok = strtok_r(NULL, "\n", &save);
    }
    memcpy(e->procs, cur, sizeof cur[0] * (size_t)ncur);
    e->npids = ncur;

    /* Classify tier. Foreground wins over everything; else active; else
     * idle by consecutive counter; long-idle at LONG_IDLE_SECS. */
    Tier tier;
    if (has_foreground) {
        tier = TIER_FOREGROUND;
        e->idle_count = 0;
    } else if (has_active) {
        tier = TIER_ACTIVE;
        e->idle_count = 0;
    } else if (ncur == 0) {
        if (e->trim_pending) {
            log_info("%s: trim finished (scope empty)", scope_name(e->path));
            e->trim_pending = false;
        }
        return; /* empty scope, nothing to grade/trim */
    } else {
        e->idle_count++;
        if (e->idle_count >= LONG_IDLE_SECS)
            tier = TIER_LONG_IDLE;
        else if (e->idle_count >= IDLE_CPU_SECS)
            tier = TIER_IDLE;
        else
            tier = TIER_ACTIVE; /* too recent to call idle yet */
    }

    if (tier != e->last_tier) {
        log_info("%s: tier %s -> %s (memlow=%llu, nice=%d)",
                 scope_name(e->path), tier_name(e->last_tier),
                 tier_name(tier), memlow_bytes(tier, total_ram),
                 tier_nice(tier));
    }
    e->last_tier = tier;

    /* Publish the memory.low target for the per-user helper to apply
     * (the daemon cannot write user-owned memory.low files). The helper
     * re-applies it each pass; values change rarely, so this is cheap. */
    {
        int w = snprintf(tier_buf + *tier_len, tier_cap - *tier_len,
                         "%s\t%llu\n", e->path, memlow_bytes(tier, total_ram));
        if (w > 0 && *tier_len + (size_t)w < tier_cap)
            *tier_len += (size_t)w;
    }

    /* Dynamic CPU nice: applied every pass, not just on tier change.
     * setpriority is idempotent and cheap, and applying it each pass
     * means a process forked or exec'd inside an already-classified
     * scope gets the right nice within a second, without relying on
     * nice inheritance across fork/exec. A scope that becomes idle gets
     * deprioritized; if it starts using CPU again it flips back to
     * ACTIVE within 1s and nice is restored -- this is what avoids the
     * ananicy/system76 terminal flaw (behavioral, not name-based
     * classification). */
    for (int i = 0; i < e->npids; i++)
        set_nice(e->procs[i].pid, tier);

    /* Working-set trimming: only under pressure, only idle/long-idle.
     * The daemon cannot read the user's /proc/<pid>/maps, so it publishes
     * the pids to trim every pass while they qualify, and the per-user
     * helper returns their address ranges; the actual MADV_COLD and the
     * TRIM_INTERVAL throttle happen in process_ranges (per uid), which
     * consumes each ranges set exactly once. Re-publishing every pass
     * means a helper on a slightly different 1s cadence never misses the
     * request (a one-shot publish could be dropped by a lost race). */
    bool trim_qualifies = psi >= PSI_THRESHOLD &&
                          (tier == TIER_IDLE || tier == TIER_LONG_IDLE);
    if (trim_qualifies) {
        if (!e->trim_pending)
            log_info("%s: trim requested: %d pid(s) (psi=%.1f%%)",
                     scope_name(e->path), e->npids, psi);
        e->trim_pending = true;
        for (int i = 0; i < e->npids; i++) {
            int w = snprintf(trim_buf + *trim_len, trim_cap - *trim_len,
                             "%d\n", e->procs[i].pid);
            if (w > 0 && *trim_len + (size_t)w < trim_cap)
                *trim_len += (size_t)w;
        }
    } else if (e->trim_pending) {
        log_info("%s: trim finished", scope_name(e->path));
        e->trim_pending = false;
    }
}
/* Read the ranges the per-user helper collected for the pids we asked to
 * trim, validate each pid belongs to the session uid (the file is
 * user-writable, so a forged pid must not let us hint another user's or
 * the kernel's memory), then process_madvise(MADV_COLD) those ranges.
 * Range line format: <pid>\t<start>-<end>,<start>-<end>,...
 *
 * Each ranges set is consumed once (the file is unlinked after reading),
 * and the caller uses the return value to throttle: last_trim is only
 * advanced when something was actually advised, so a helper that is one
 * cycle behind is retried on the next pass instead of waited out.
 * Returns true if any bytes were advised. */
static bool process_ranges(const char *uid_str) {
    unsigned long uid = strtoul(uid_str, NULL, 10);
    char path[64];
    char line[1 << 16];
    FILE *fp;
    snprintf(path, sizeof path, "/tmp/aerolike-ranges-%s", uid_str);
    fp = fopen(path, "r");
    if (!fp) return false;

    bool advised = false;
    long long total_advised = 0;
    int advised_pids = 0;
    static time_t err_last = 0;   /* ratelimit for repeated MADV_COLD errors */
    while (fgets(line, sizeof line, fp)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        pid_t pid = (pid_t)strtol(line, NULL, 10);
        if (pid <= 0 || !pid_belongs_to(pid, uid)) continue;

        char *r = tab + 1;
        struct iovec iov[MAX_IOVEC];
        int n = 0;
        char *save = NULL;
        for (char *tok = strtok_r(r, ",", &save);
             tok && n < MAX_IOVEC;
             tok = strtok_r(NULL, ",", &save)) {
            unsigned long s = 0, e = 0;
            if (sscanf(tok, "%lx-%lx", &s, &e) == 2 && e > s) {
                iov[n].iov_base = (void *)s;
                iov[n].iov_len = e - s;
                n++;
            }
        }
        if (n == 0) continue;

        int fd = (int)syscall(SYS_pidfd_open, pid, 0);
        if (fd < 0) {
            log_ratelimited(&err_last, 30, "trim: pidfd_open(%d) failed: %s",
                            pid, strerror(errno));
            continue;
        }
        long rc = syscall(SYS_process_madvise, fd, iov, (unsigned long)n,
                          MADV_COLD, 0);
        close(fd);
        if (rc > 0) {
            advised = true;
            total_advised += rc;
            advised_pids++;
        } else if (rc < 0) {
            log_ratelimited(&err_last, 30,
                            "trim: process_madvise(pid %d, %d ranges) failed: %s",
                            pid, n, strerror(errno));
        }
    }
    if (advised)
        log_info("trim: advised %lld bytes MADV_COLD across %d pid(s)",
                 total_advised, advised_pids);

    /* Consume the ranges file (the helper rewrites it every second), so a
     * stale set is advised at most once instead of re-advised forever. The
     * helper writes atomically (tmp+rename), and if this unlink races its
     * next rename the worst case is one skipped set that the next pass
     * already has fresh data for. */
    fclose(fp);
    unlink(path);
    return advised;
}

/* ---------- boost the desktop environment's CPU priority ---------- */

/* Apply NICE_DESKTOP to every process in desktop.slice scopes. This is
 * CPU-only: we never touch the DE's memory (no memory.low, no MADV_COLD).
 * The DE must always win scheduling to respond to input and composite
 * frames, even when a heavy app is using all cores. Called every pass;
 * setpriority is idempotent so re-applying is cheap. */
static void boost_desktop(void) {
    DIR *d = opendir("/sys/fs/cgroup/user.slice");
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *nm = ent->d_name;
        if (strncmp(nm, "user-", 5) != 0) continue;
        char uid[32];
        if (!user_uid(nm, uid, sizeof uid)) continue;
        char base[PATH_MAX];
        snprintf(base, sizeof base,
                 "/sys/fs/cgroup/user.slice/%s/user@%s.service/desktop.slice",
                 nm, uid);

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

            /* Read cgroup.procs and boost each pid. */
            char procs[65536];
            snprintf(procs, sizeof procs, "%s/cgroup.procs", scope);
            int fd = open(procs, O_RDONLY);
            if (fd < 0) continue;
            ssize_t n = read(fd, procs, sizeof procs - 1);
            close(fd);
            if (n <= 0) continue;
            procs[n] = 0;
            char *save = NULL;
            char *tok = strtok_r(procs, "\n", &save);
            while (tok) {
                pid_t pid = (pid_t)strtol(tok, NULL, 10);
                if (pid > 0) {
                    errno = 0;
                    (void)setpriority(PRIO_PROCESS, pid, NICE_DESKTOP);
                }
                tok = strtok_r(NULL, "\n", &save);
            }
        }
        closedir(sd);
    }
    closedir(d);
}

/* ---------- enumerate scopes across all users ---------- */

static void walk_users(double psi, unsigned long long total_ram) {
    SessionInfo sessions[MAX_SESSIONS];
    int nsess = find_sessions(sessions, MAX_SESSIONS);

    /* Log foreground window changes (per user) so the classification is
     * observable; this only fires when the active window actually changes. */
    static char fg_uid[MAX_SESSIONS][32];
    static pid_t fg_pid[MAX_SESSIONS];
    static int fg_n = 0;
    for (int i = 0; i < nsess; i++) {
        int slot = -1;
        for (int j = 0; j < fg_n; j++)
            if (strcmp(fg_uid[j], sessions[i].uid) == 0) { slot = j; break; }
        if (slot < 0 && fg_n < MAX_SESSIONS) {
            snprintf(fg_uid[fg_n], sizeof fg_uid[0], "%s", sessions[i].uid);
            fg_pid[fg_n] = -1;   /* force a log of the very first state */
            slot = fg_n++;
        }
        if (slot >= 0 && fg_pid[slot] != sessions[i].fg) {
            log_info("user %s: foreground pid %d%s",
                     sessions[i].uid, sessions[i].fg,
                     sessions[i].fg ? "" : " (none focused)");
            fg_pid[slot] = sessions[i].fg;
        }
    }

    DIR *d = opendir("/sys/fs/cgroup/user.slice");
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *nm = ent->d_name;
        if (strncmp(nm, "user-", 5) != 0) continue;
        char uid[32];
        if (!user_uid(nm, uid, sizeof uid)) continue;
        char base[PATH_MAX];
        snprintf(base, sizeof base,
                 "/sys/fs/cgroup/user.slice/%s/user@%s.service/app.slice",
                 nm, uid);

        char tier_buf[1 << 16];
        char trim_buf[1 << 16];
        size_t tier_len = 0, trim_len = 0;

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
            if (e)
                process_scope(e, sessions, nsess, psi, total_ram,
                              tier_buf, sizeof tier_buf, &tier_len,
                              trim_buf, sizeof trim_buf, &trim_len);
        }
        closedir(sd);

        write_uid_file("tier", uid, tier_buf, tier_len);
        write_uid_file("trim", uid, trim_buf, trim_len);

        /* Only process a trim request when there is one, at most every
         * TRIM_INTERVAL per uid, and only advance the throttle clock when
         * something was actually advised -- so a helper that is a cycle
         * behind is retried next pass instead of waiting out the
         * interval. */
        if (trim_len > 0) {
            int slot = uid_trim_slot(uid);
            if (slot >= 0 &&
                time(NULL) - uid_trim_last[slot] >= TRIM_INTERVAL &&
                process_ranges(uid)) {
                uid_trim_last[slot] = time(NULL);
            }
        }
    }
    closedir(d);
}

/* ---------- dynamic swappiness + swap-device awareness ---------- */

/* Base swappiness by RAM. This is a workload-adaptive knob now, so it
 * lives here (set every pass) rather than in the one-shot
 * memory-tweaks service. Low RAM eagerly swaps anon to zram so file
 * cache stays warm; high RAM reclaims cache first (Windows-like), where
 * eager zram-swap would be wasted compression work. */
static int base_swappiness(unsigned long long total_ram) {
    unsigned long long meg = total_ram / (1024ULL * 1024ULL);
    if (meg <= 8000ULL)  return 180;
    if (meg <= 16000ULL) return 120;
    return 80;
}

/* zram fill ratio in percent = mem_used_total / disksize. Returns -1 if
 * zram is absent or unreadable. mm_stat fields: orig_size compr_size
 * mem_used mem_limit mem_used_max same_pages compacted huge_pages. */
static double zram_fill_pct(void) {
    char buf[256];
    int fd = open("/sys/block/zram0/mm_stat", O_RDONLY);
    if (fd < 0) return -1.0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1.0;
    buf[n] = 0;
    unsigned long long orig = 0, compr = 0, used = 0;
    if (sscanf(buf, "%llu %llu %llu", &orig, &compr, &used) != 3)
        return -1.0;

    fd = open("/sys/block/zram0/disksize", O_RDONLY);
    if (fd < 0) return -1.0;
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1.0;
    buf[n] = 0;
    unsigned long long disksize = strtoull(buf, NULL, 10);
    if (disksize == 0) return -1.0;
    return (double)used * 100.0 / (double)disksize;
}

#define SWAPPINESS_FILL_START 50.0  /* fill% at which we start lowering */
#define SWAPPINESS_FILL_FULL  90.0  /* fill% at which we hit the floor */
#define SWAPPINESS_FLOOR_ZRAMONLY 40  /* zram-only: stop feeding a full store */
#define SWAPPINESS_FLOOR_DISK    100 /* with a disk-swap exit, keep spilling anon */

/* Probe /proc/swaps (or a fixture path for testing) for a non-zram swap
 * device -- a disk swapfile or partition (shipped as
 * swapfile-setup.service + var-swapfile.swap). Two signals come out:
 *   - present: a disk-swap device exists. Controls the zram-fill
 *     swappiness floor: with one, the floor must stay higher so the
 *     kernel keeps spilling cold anon to disk instead of dropping
 *     cache -- the disk swap is the cold-page exit that a zram-only
 *     floor would otherwise suppress.
 *   - used_kb: the largest Used column of any non-zram device (KB, as
 *     reported by /proc/swaps). This is the "is the disk swap actually
 *     holding pages" signal that gates page-cluster readahead:
 *     presence alone is not enough, because swap-in reads only happen
 *     once pages resident on disk are faulted back. */
static void probe_disk_swap(const char *path, bool *present, long long *used_kb) {
    *present = false;
    *used_kb = 0;
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char line[1024];
    while (fgets(line, sizeof line, fp)) {
        char dev[256] = "", typ[32] = "";
        unsigned long long size = 0, used = 0;
        int pri = 0;
        /* /proc/swaps: Filename Type Size Used Priority */
        if (sscanf(line, "%255s %31s %llu %llu %d",
                   dev, typ, &size, &used, &pri) != 5)
            continue;
        if (dev[0] != '/') continue;              /* skip header line */
        if (strncmp(dev, "/dev/zram", 9) == 0) continue;  /* not disk */
        *present = true;
        if ((long long)used > *used_kb) *used_kb = (long long)used;
    }
    fclose(fp);
}

/* Write the value to /proc/sys/vm/page-cluster and log a transition.
 * No-op (with no log noise) if the run lacks permission. Resets the
 * debounce counters so the next flip needs a fresh sustained run. */
static void write_page_cluster(int value, const char *why) {
    int fd = open("/proc/sys/vm/page-cluster", O_WRONLY);
    if (fd < 0) return;
    char buf[8];
    int len = snprintf(buf, sizeof buf, "%d", value);
    (void)write(fd, buf, (size_t)len);
    close(fd);
    page_cluster = value;
    page_used_up = 0;
    page_used_dn = 0;
    log_info("vm.page-cluster = %d (%s)", value, why);
}

/* Lower swappiness as zram fills so a nearly-full compressed store stops
 * absorbing anon pages (which thrashes the store) and the kernel
 * reclaims file cache instead. Returns base when zram is absent. */
static int swappiness_for_fill(double fill, int base, int floor) {
    if (fill < 0.0 || fill <= SWAPPINESS_FILL_START) return base;
    double t = (fill - SWAPPINESS_FILL_START) /
               (SWAPPINESS_FILL_FULL - SWAPPINESS_FILL_START);
    if (t > 1.0) t = 1.0;
    return (int)(base - (double)(base - floor) * t + 0.5);
}

/* ---------- main ---------- */

static volatile sig_atomic_t running = 1;
static void on_signal(int sig) { (void)sig; running = 0; }

/* Warn on startup if CAP_SYS_NICE is missing -- without it every
 * process_madvise(MADV_COLD) would fail, so it is worth surfacing. */
static bool have_sys_nice(void) {
    FILE *fp = fopen("/proc/self/status", "r");
    if (!fp) return false;
    char line[256];
    unsigned long long capeff = 0;
    while (fgets(line, sizeof line, fp)) {
        if (strncmp(line, "CapEff:", 7) == 0) {
            capeff = strtoull(line + 7, NULL, 16);
            break;
        }
    }
    fclose(fp);
    /* CAP_SYS_NICE == bit 23 (0x800000), e.g. CapEff=0000000000800000. */
    return (capeff >> 23) & 1ULL;
}

int main(void) {
    unsigned long long total_ram = total_ram_bytes();
    int base_sw = base_swappiness(total_ram);
    int swappiness = -1;
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    log_info("started: total RAM %llu bytes (%.1f GiB), CAP_SYS_NICE=%s",
             total_ram, total_ram / 1073741824.0,
             have_sys_nice() ? "yes" : "NO -- process_madvise will fail");

    while (running) {
        double psi = psi_memory_some();
        /* Drop state for scopes that have exited. */
        prune_scopes();
        /* Boost the DE's CPU priority every pass (cheap, idempotent). */
        boost_desktop();
        /* Gradation + trim requests run every pass; actual MADV_COLD is
         * gated by psi inside process_ranges via the helper's data. */
        walk_users(psi, total_ram);

        /* Swap-device awareness: a disk swapfile/partition (shipped as
         * swapfile-setup.service + var-swapfile.swap) changes two policy
         * knobs. Per-pass /proc/swaps read is tiny; it also catches the
         * swap unit activating around the same time and a manual swapon.
         * 'disk' (presence) drives the swappiness floor below; swap
         * USAGE (disk_used_kb) drives the page-cluster flip below, so
         * readahead tracks actual swap-in demand rather than the mere
         * existence of a disk swap. */
        bool disk = false;
        long long disk_used_kb = 0;
        probe_disk_swap("/proc/swaps", &disk, &disk_used_kb);

        /* page-cluster: 0 for zram-only (no readahead on RAM-backed
         * swap), 3 for a disk swap that is actually being used
         * (readahead on swap-in). Presence alone is not enough: disk
         * swap-in reads only happen once pages are resident on the disk
         * swap. Debounced both ways so a single pushed-out page cannot
         * flap a global knob. */
        if (disk_used_kb > PAGE_CLUSTER_USED_MIN) {
            if (page_used_up < PAGE_CLUSTER_UP_DEBOUNCE) page_used_up++;
            page_used_dn = 0;
        } else {
            if (page_used_dn < PAGE_CLUSTER_DN_DEBOUNCE) page_used_dn++;
            page_used_up = 0;
        }

        if (page_cluster == 3 && page_used_dn >= PAGE_CLUSTER_DN_DEBOUNCE) {
            write_page_cluster(0, "disk swap drained");
        } else if (page_cluster == 0 && page_used_up >= PAGE_CLUSTER_UP_DEBOUNCE &&
                   disk_used_kb > PAGE_CLUSTER_USED_MIN) {
            write_page_cluster(3, "disk swap in use");
        }

        /* Dynamic swappiness: react to zram fill, not just RAM at boot.
         * Writes only on change; the value drifts back to base as pages
         * leave the compressed store. */
        int floor = disk ? SWAPPINESS_FLOOR_DISK : SWAPPINESS_FLOOR_ZRAMONLY;
        int want = swappiness_for_fill(zram_fill_pct(), base_sw, floor);
        if (want != swappiness) {
            int fd = open("/proc/sys/vm/swappiness", O_WRONLY);
            if (fd >= 0) {
                char buf[32];
                int len = snprintf(buf, sizeof buf, "%d", want);
                (void)write(fd, buf, (size_t)len);
                close(fd);
                log_info("vm.swappiness = %d (zram-fill adaptive%s)", want,
                         disk ? ", disk swap present" : "");
                swappiness = want;
            }
        }
        sleep(SLEEP_SECS);
    }
    log_info("stopped");
    return 0;
}
