/*
 * libappimage-scope: LD_PRELOAD shim that runs AppImages (and the sas
 * AppImage sandbox, and select XFCE apps) inside their own systemd user
 * scope (app.slice) so oomd can rank/kill each one independently and
 * the app.slice MemoryHigh cap applies.
 *
 * Intercepts the full exec family (execve, execv, execvp, execvpe,
 * execl/execlp/execle, execveat) and posix_spawn/posix_spawnp. glibc
 * routes execv/execvp/execl/execle and posix_spawn through internal
 * helpers that call __execve directly, which cannot be interposed, so
 * each public entry point must be wrapped individually. If the target
 * file is an AppImage (ELF + "AI"/"RI"/"AB" magic at offset 8), the sas
 * sandbox launcher (/usr/bin/sas), or a listed XFCE app (xfsettingsd,
 * xfce4-power-manager, thunar, etc.), rewrites the exec to:
 *
 *   systemd-run --user --scope --slice=app.slice -- <target> [args]
 *
 * Scoping sas pulls its whole process tree (sas -> bwrap -> AppRun ->
 * FUSE daemon) into one cgroup, so the sandbox becomes a single
 * oomd-manageable unit instead of an untracked part of the session blob.
 *
 * Scoping XFCE apps moves disposable GUI utilities (settings manager +
 * all its frontends, power manager, notifications, screensaver,
 * screenshooter, appfinder, terminal, file manager, session settings/
 * logout/about, tray applets, nm-connection-editor, pavucontrol,
 * xarchiver, blueman manager/adapters, ccsm, qt5ct/qt6ct,
 * kvantummanager, emerald-theme-manager) out of the protected
 * desktop.slice DE blob into killable app.slice scopes, so a runaway
 * settings daemon or file manager can be killed without touching the
 * desktop. The core DE (xfce4-session, xfce4-panel, compiz/emerald) is
 * deliberately NOT in this list and stays protected. The polkit auth
 * agent is also excluded: it's a critical daemon, not a disposable app,
 * and killing it would break all graphical elevation prompts.
 *
 * The recursion guard (APPIMAGE_SCOPED=1 + stripping our own lib from
 * LD_PRELOAD when spawning systemd-run) prevents the hook from looping.
 * Any failure falls back to the real exec.
 *
 * Detection is by magic bytes only (not extension), covering the
 * squashfs runtime (Type 2, "AI"), the dwarfs runtime ("AI"), and the
 * ISO/runtime variants ("RI", "AB").
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <spawn.h>
#include <stdarg.h>
#include <sys/types.h>

extern char **environ;

typedef int (*execve_t)(const char *, char *const[], char *const[]);
typedef int (*execveat_t)(int, const char *, char *const[], char *const[], int);
typedef int (*execv_t)(const char *, char *const[]);
typedef int (*execvp_t)(const char *, char *const[]);
typedef int (*execvpe_t)(const char *, char *const[], char *const[]);
typedef int (*posix_spawn_t)(pid_t *, const char *,
                             const posix_spawn_file_actions_t *,
                             const posix_spawnattr_t *,
                             char *const[], char *const[]);
typedef int (*posix_spawnp_t)(pid_t *, const char *,
                              const posix_spawn_file_actions_t *,
                              const posix_spawnattr_t *,
                              char *const[], char *const[]);

static int is_appimage(const char *path) {
    unsigned char buf[10];
    int fd;
    ssize_t n;
    if (!path) return 0;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    n = read(fd, buf, 10);
    close(fd);
    if (n < 10) return 0;
    /* bytes 1-3 == "ELF", bytes 8-9 in {AI,RI,AB} (AppImage magic) */
    if (!(buf[1]=='E' && buf[2]=='L' && buf[3]=='F')) return 0;
    return (buf[8]=='A' && buf[9]=='I') ||
           (buf[8]=='R' && buf[9]=='I') ||
           (buf[8]=='A' && buf[9]=='B');
}

static int is_sas(const char *path) {
    if (!path) return 0;
    if (strcmp(path, "/usr/bin/sas") == 0) return 1;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return strcmp(base, "sas") == 0;
}

/* XFCE apps that should get their own killable scope instead of living
 * in the desktop.slice DE blob. These are disposable utilities that can
 * be safely killed and restarted by the user, unlike the core DE
 * (session/panel/compiz) which stays protected in desktop.slice. */
static int is_de_app(const char *path) {
    static const char *apps[] = {
        "xfce4-settings-manager",
        "xfsettingsd",
        "xfce4-appearance-settings",
        "xfce4-display-settings",
        "xfce4-keyboard-settings",
        "xfce4-mouse-settings",
        "xfce4-mime-settings",
        "xfce4-print-settings",
        "xfce4-session-settings",
        "xfce4-session-logout",
        "xfce4-about",
        "xfce4-power-manager",
        "xfce4-notifyd",
        "xfce4-screenshooter",
        "xfce4-screensaver",
        "xfce4-appfinder",
        "xfce4-terminal",
        "thunar",
        "nm-applet",
        "nm-connection-editor",
        "blueman-applet",
        "blueman-manager",
        "blueman-adapters",
        "pavucontrol",
        "xarchiver",
        "ccsm",
        "qt5ct",
        "qt6ct",
        "kvantummanager",
        "emerald-theme-manager",
        NULL
    };
    const char *base;
    int i;
    if (!path) return 0;
    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    for (i = 0; apps[i]; i++) {
        if (strcmp(base, apps[i]) == 0) return 1;
    }
    return 0;
}

static int count_argv(char *const argv[]) {
    int n = 0;
    while (argv && argv[n]) n++;
    return n;
}

static int count_envp(char *const envp[]) {
    int n = 0;
    while (envp && envp[n]) n++;
    return n;
}

/* Build a new envp for systemd-run: strip our own lib from LD_PRELOAD
 * and add the recursion guard. Other LD_PRELOAD entries are preserved. */
static int scrub_envp(char **out, char *const envp[]) {
    int n = count_envp(envp);
    int o = 0;
    char buf[8192];
    for (int i = 0; i < n; i++) {
        if (!envp[i]) continue;
        if (strncmp(envp[i], "LD_PRELOAD=", 11) == 0) {
            const char *v = envp[i] + 11;
            const char *p = v;
            size_t len = 0;
            int wrote = 0;
            while (*p) {
                const char *end = strchr(p, ':');
                size_t part = end ? (size_t)(end - p) : strlen(p);
                int is_self = (strstr(p, "appimage-scope") != NULL);
                if (!is_self) {
                    if (len + part + 2 < sizeof buf) {
                        if (wrote) buf[len++] = ':';
                        memcpy(buf + len, p, part);
                        len += part;
                        wrote = 1;
                    }
                }
                if (!end) break;
                p = end + 1;
            }
            buf[len] = 0;
            if (len > 0) {
                char tmp[8192];
                snprintf(tmp, sizeof tmp, "LD_PRELOAD=%.*s", (int)len, buf);
                out[o++] = strdup(tmp);
            }
            continue;
        }
        out[o++] = strdup(envp[i]);
    }
    out[o++] = strdup("APPIMAGE_SCOPED=1");
    out[o] = NULL;
    return o;
}

/* Resolve a command name to its full path using PATH, exactly like
 * execvp/posix_spawnp do (first X_OK entry wins). This matches the
 * binary the caller would actually launch, avoiding guessing. Returns a
 * strdup'd full path, or NULL if not found / not executable. */
static char *resolve_in_path(const char *name) {
    const char *path;
    char *dup, *dir, *save = NULL;
    if (!name || !*name) return NULL;
    if (strchr(name, '/')) {
        if (access(name, X_OK) == 0) return strdup(name);
        return NULL;
    }
    path = getenv("PATH");
    if (!path || !*path) return NULL;
    dup = strdup(path);
    if (!dup) return NULL;
    for (dir = strtok_r(dup, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        if (*dir == '\0') dir = ".";
        size_t len = strlen(dir) + 1 + strlen(name) + 1;
        char *cand = malloc(len);
        if (!cand) break;
        snprintf(cand, len, "%s/%s", dir, name);
        if (access(cand, X_OK) == 0) { free(dup); return cand; }
        free(cand);
    }
    free(dup);
    return NULL;
}

/* Build the argv/envp that runs <target> [args] through systemd-run in
 * app.slice. Returns the new argv (caller must free) and fills newenvp
 * (each entry strdup'd; caller must free). target must already be a
 * resolved, verified full path. */
static char **build_redirect(int argc, char *const argv[], const char *target,
                             char **newenvp, char *const envp[]) {
    char **newargv = calloc(argc + 8, sizeof(char *));
    if (!newargv) return NULL;
    int i = 0;
    newargv[i++] = "systemd-run";
    newargv[i++] = "--user";
    newargv[i++] = "--scope";
    newargv[i++] = "--slice=app.slice";
    newargv[i++] = "--";
    newargv[i++] = (char *)target;
    for (int j = 1; j < argc; j++) newargv[i++] = argv[j];
    newargv[i] = NULL;
    scrub_envp(newenvp, envp);
    return newargv;
}

/* Only redirect when the target is a real executable. Callers that
 * search PATH (execvp, posix_spawnp, GLib) iterate by calling execve
 * on each candidate; the first candidate may be a bogus path (e.g. a
 * nonexistent AppImage mount bin dir). If we redirected that to
 * systemd-run verbatim, systemd-run would fail on the dead path and
 * the caller's loop -- now replaced by systemd-run -- would never
 * reach the real binary. Falling through to real execve returns
 * ENOENT so the PATH search continues. PATH itself is left intact;
 * only the execve target is validated. */
static int redirect_exec(const char *path, char *const argv[], char *const envp[]) {
    static execve_t real_execve = NULL;
    if (!real_execve) real_execve = (execve_t)dlsym(RTLD_NEXT, "execve");
    if (!real_execve) { errno = ENOSYS; return -1; }

    if (access(path, X_OK) != 0) {
        return real_execve(path, argv, envp);
    }

    int argc = count_argv(argv);
    int envc = count_envp(envp);

    char **newenvp = calloc(envc + 8, sizeof(char *));
    if (!newenvp) { errno = ENOMEM; return -1; }
    char **newargv = build_redirect(argc, argv, path, newenvp, envp);
    if (!newargv) { free(newenvp); errno = ENOMEM; return -1; }

    int nenv = count_envp(newenvp);

    real_execve("/usr/bin/systemd-run", newargv, newenvp);
    /* exec failed; fall through and let caller run the original */
    int saved = errno;
    for (int j = 0; j < nenv; j++) free(newenvp[j]);
    free(newargv); free(newenvp);
    errno = saved;
    return -1;
}

/* Decide whether a spawn/execvp-style target (which may be a bare
 * command name resolved via PATH) is something we scope. Resolves bare
 * names so AppImages reachable only via PATH are detected. */
static int target_matches(const char *file) {
    if (!file || !*file) return 0;
    if (is_appimage(file) || is_sas(file) || is_de_app(file)) return 1;
    if (strchr(file, '/')) return 0;
    char *resolved = resolve_in_path(file);
    if (!resolved) return 0;
    int m = is_appimage(resolved) || is_sas(resolved) || is_de_app(resolved);
    free(resolved);
    return m;
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    static execve_t real = NULL;
    if (!real) real = (execve_t)dlsym(RTLD_NEXT, "execve");
    if (!real) { errno = ENOSYS; return -1; }
    if (getenv("APPIMAGE_SCOPED")) return real(path, argv, envp);
    if (is_appimage(path) || is_sas(path) || is_de_app(path)) {
        redirect_exec(path, argv, envp);
    }
    return real(path, argv, envp);
}

int execv(const char *path, char *const argv[]) {
    static execv_t real = NULL;
    if (!real) real = (execv_t)dlsym(RTLD_NEXT, "execv");
    if (!real) { errno = ENOSYS; return -1; }
    if (getenv("APPIMAGE_SCOPED")) return real(path, argv);
    if (target_matches(path)) {
        char *resolved = resolve_in_path(path);
        if (resolved) {
            redirect_exec(resolved, argv, environ);
            free(resolved);
        }
    }
    return real(path, argv);
}

int execvp(const char *file, char *const argv[]) {
    static execvp_t real = NULL;
    if (!real) real = (execvp_t)dlsym(RTLD_NEXT, "execvp");
    if (!real) { errno = ENOSYS; return -1; }
    if (getenv("APPIMAGE_SCOPED")) return real(file, argv);
    if (target_matches(file)) {
        char *resolved = resolve_in_path(file);
        if (resolved) {
            redirect_exec(resolved, argv, environ);
            free(resolved);
        }
    }
    return real(file, argv);
}

int execvpe(const char *file, char *const argv[], char *const envp[]) {
    static execvpe_t real = NULL;
    if (!real) real = (execvpe_t)dlsym(RTLD_NEXT, "execvpe");
    if (!real) { errno = ENOSYS; return -1; }
    if (getenv("APPIMAGE_SCOPED")) return real(file, argv, envp);
    if (target_matches(file)) {
        char *resolved = resolve_in_path(file);
        if (resolved) {
            redirect_exec(resolved, argv, envp);
            free(resolved);
        }
    }
    return real(file, argv, envp);
}

/* Shared redirect for posix_spawn/posix_spawnp. Resolves the target (a
 * bare name gets PATH resolution matching the caller's semantics),
 * then spawns systemd-run instead of the target via the real
 * posix_spawnp, so the app ends up in its own app.slice scope. */
static int redirect_spawn(pid_t *pid, const char *file,
                          const posix_spawn_file_actions_t *file_actions,
                          const posix_spawnattr_t *attrp,
                          char *const argv[], char *const envp[]) {
    static posix_spawnp_t real_spawnp = NULL;
    if (!real_spawnp) real_spawnp = (posix_spawnp_t)dlsym(RTLD_NEXT, "posix_spawnp");
    if (!real_spawnp) return EINVAL;

    char *target = resolve_in_path(file);
    if (!target) return EINVAL;

    int argc = count_argv(argv);
    int envc = count_envp(envp);
    char **newenvp = calloc(envc + 8, sizeof(char *));
    char **newargv = NULL;
    if (newenvp) newargv = build_redirect(argc, argv, target, newenvp, envp);
    if (!newargv) { free(newenvp); free(target); return ENOMEM; }

    int nenv = count_envp(newenvp);
    int ret = real_spawnp(pid, "/usr/bin/systemd-run", file_actions, attrp,
                          newargv, newenvp);
    for (int j = 0; j < nenv; j++) free(newenvp[j]);
    free(newargv); free(newenvp);
    free(target);
    return ret;
}

int posix_spawn(pid_t *pid, const char *file,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const argv[], char *const envp[]) {
    static posix_spawn_t real = NULL;
    if (!real) real = (posix_spawn_t)dlsym(RTLD_NEXT, "posix_spawn");
    if (!real) return EINVAL;
    if (getenv("APPIMAGE_SCOPED")) return real(pid, file, file_actions, attrp, argv, envp);
    if (target_matches(file)) {
        int r = redirect_spawn(pid, file, file_actions, attrp, argv, envp);
        if (r == 0) return 0;
    }
    return real(pid, file, file_actions, attrp, argv, envp);
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp,
                 char *const argv[], char *const envp[]) {
    static posix_spawnp_t real = NULL;
    if (!real) real = (posix_spawnp_t)dlsym(RTLD_NEXT, "posix_spawnp");
    if (!real) return EINVAL;
    if (getenv("APPIMAGE_SCOPED")) return real(pid, file, file_actions, attrp, argv, envp);
    if (target_matches(file)) {
        int r = redirect_spawn(pid, file, file_actions, attrp, argv, envp);
        if (r == 0) return 0;
    }
    return real(pid, file, file_actions, attrp, argv, envp);
}

int execveat(int dirfd, const char *path, char *const argv[], char *const envp[], int flags) {
    static execveat_t real = NULL;
    if (!real) real = (execveat_t)dlsym(RTLD_NEXT, "execveat");
    if (!real) { errno = ENOSYS; return -1; }
    if (getenv("APPIMAGE_SCOPED")) return real(dirfd, path, argv, envp, flags);
    if (dirfd == AT_FDCWD && (is_appimage(path) || is_sas(path) || is_de_app(path))) {
        redirect_exec(path, argv, envp);
    }
    return real(dirfd, path, argv, envp, flags);
}

/* Variadic exec* wrappers: build the argv array from the varargs and
 * delegate to the vector forms above (which own the redirect logic). */

static char **build_exec_argv(const char *arg, va_list ap) {
    int n = 0, cap = 8;
    char **argv = malloc(cap * sizeof(char *));
    if (!argv) return NULL;
    argv[n++] = (char *)arg;
    for (;;) {
        const char *s = va_arg(ap, const char *);
        if (!s) break;
        if (n + 1 >= cap) {
            cap *= 2;
            char **na = realloc(argv, cap * sizeof(char *));
            if (!na) { free(argv); return NULL; }
            argv = na;
        }
        argv[n++] = (char *)s;
    }
    argv[n] = NULL;
    return argv;
}

int execl(const char *path, const char *arg, ...) {
    va_list ap;
    char **argv;
    va_start(ap, arg);
    argv = build_exec_argv(arg, ap);
    va_end(ap);
    if (!argv) { errno = ENOMEM; return -1; }
    int r = execv(path, argv);
    free(argv);
    return r;
}

int execlp(const char *file, const char *arg, ...) {
    va_list ap;
    char **argv;
    va_start(ap, arg);
    argv = build_exec_argv(arg, ap);
    va_end(ap);
    if (!argv) { errno = ENOMEM; return -1; }
    int r = execvp(file, argv);
    free(argv);
    return r;
}

int execle(const char *path, const char *arg, ...) {
    va_list ap;
    char **argv;
    char **envp;
    va_start(ap, arg);
    argv = build_exec_argv(arg, ap);
    envp = va_arg(ap, char **);
    va_end(ap);
    if (!argv) { errno = ENOMEM; return -1; }
    int r = execvpe(path, argv, envp);
    free(argv);
    return r;
}
