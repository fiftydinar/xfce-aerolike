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
 * Runs as a root system service because:
 *   - process_madvise(MADV_COLD) cross-process requires CAP_SYS_NICE.
 *   - One daemon coordinates trimming/gradation across ALL sessions.
 * The service drops to CAP_SYS_NICE + NoNewPrivileges.
 *
 * Behavior:
 *   - Polls /proc/pressure/memory (PSI). Gradation runs every pass;
 *     trimming only when pressure is actually building (PSI >= 10).
 *   - Enumerates app.slice scopes for every user session. desktop.slice
 *     (DE) and protected services are structurally excluded.
 *   - Each scope is classified into a tier:
 *       FOREGROUND: owns the active X window (per-display)
 *       ACTIVE:     any CPU activity in the last ~60s
 *       IDLE:       no CPU for >= IDLE_CPU_SECS (60s)
 *       LONG_IDLE:  no CPU for >= LONG_IDLE_SECS (5min)
 *   - memory.low is written per tier (RAM %): foreground highest, idle
 *     lowest, so reclaim order is graded like Windows.
 *   - CPU nice is set per tier on change: foreground/active get normal
 *     nice, idle/long-idle get deprioritized. Behavioral classification
 *     (what the scope is DOING, not its name) means a terminal running a
 *     heavy CLI stays active/normal -- it only gets deprioritized when
 *     genuinely idle. This avoids the static-classifier flaw of
 *     ananicy-cpp/system76-scheduler.
 *   - MADV_COLD trims IDLE/LONG_IDLE scopes under pressure. Pages stay
 *     in RAM (inactive list) -> cheap resume, not zram decompression.
 *
 * Active-window detection is delegated to a per-user systemd service
 * that runs xdotool inside the graphical session (where DISPLAY and
 * XAUTHORITY are set) and writes the active window PID to
 * /tmp/aerolike-fg-<uid>. The root daemon reads that file: it drops
 * CAP_SYS_PTRACE and CAP_DAC_OVERRIDE, so it cannot reach the user's X
 * auth itself.
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
    time_t last_trim;      /* last MADV_COLD pass, for throttling */
    Tier last_tier;
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
    pid_t fg;   /* active X window PID, reported by per-user helper */
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

/* ---------- memory.low gradation ---------- */

static void set_memory_low(const char *scope_path, Tier tier,
                           unsigned long long total_ram) {
    double frac;
    switch (tier) {
    case TIER_FOREGROUND: frac = MEMLOW_FOREGROUND; break;
    case TIER_ACTIVE:     frac = MEMLOW_ACTIVE;     break;
    case TIER_IDLE:       frac = MEMLOW_IDLE;       break;
    case TIER_LONG_IDLE:  frac = MEMLOW_LONG_IDLE;  break;
    default:              return;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/memory.low", scope_path);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;
    char buf[64];
    unsigned long long bytes = (unsigned long long)(total_ram * frac);
    int len = snprintf(buf, sizeof buf, "%llu", bytes);
    write(fd, buf, (size_t)len);
    close(fd);
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

/* ---------- classify + grade + trim one scope ---------- */

static void process_scope(ScopeEnt *e, SessionInfo *sessions, int nsess,
                          double psi, unsigned long long total_ram) {
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
        tier = e->last_tier;
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

    /* Memory priority gradation: write memory.low on tier change
     * (per-cgroup, so a process spawned into the scope automatically
     * inherits it). CPU nice is applied EVERY pass below instead. */
    if (tier != e->last_tier) {
        set_memory_low(e->path, tier, total_ram);
        e->last_tier = tier;
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

    /* Working-set trimming: only under pressure, only idle/long-idle,
     * and at most every TRIM_INTERVAL per scope. cold_madvise rescans
     * /proc/<pid>/maps per call; throttling avoids redoing that every
     * second for the whole idle set under sustained pressure. */
    if (psi >= PSI_THRESHOLD &&
        (tier == TIER_IDLE || tier == TIER_LONG_IDLE) &&
        time(NULL) - e->last_trim >= TRIM_INTERVAL) {
        for (int i = 0; i < e->npids; i++)
            cold_madvise(e->procs[i].pid);
        e->last_trim = time(NULL);
    }
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
            if (e) process_scope(e, sessions, nsess, psi, total_ram);
        }
        closedir(sd);
    }
    closedir(d);
}

/* ---------- main ---------- */

static volatile sig_atomic_t running = 1;
static void on_signal(int sig) { (void)sig; running = 0; }

int main(void) {
    unsigned long long total_ram = total_ram_bytes();
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    while (running) {
        double psi = psi_memory_some();
        /* Drop state for scopes that have exited. */
        prune_scopes();
        /* Boost the DE's CPU priority every pass (cheap, idempotent). */
        boost_desktop();
        /* Gradation runs every pass; trimming is gated by psi inside
         * process_scope (only acts on idle tiers when psi >= threshold). */
        walk_users(psi, total_ram);
        sleep(SLEEP_SECS);
    }
    return 0;
}
