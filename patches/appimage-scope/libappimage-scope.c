/*
 * libappimage-scope: LD_PRELOAD shim that runs AppImages (and the sas
 * AppImage sandbox) inside their own systemd user scope (app.slice) so
 * oomd can rank/kill each one independently and the app.slice
 * MemoryHigh cap applies.
 *
 * Intercepts execve/execveat. If the target file is an AppImage
 * (ELF + "AI"/"RI"/"AB" magic at offset 8), or the sas sandbox launcher
 * (/usr/bin/sas), rewrites the exec to:
 *
 *   systemd-run --user --scope --slice=app.slice -- <target> [args]
 *
 * Scoping sas pulls its whole process tree (sas -> bwrap -> AppRun ->
 * FUSE daemon) into one cgroup, so the sandbox becomes a single
 * oomd-manageable unit instead of an untracked part of the session blob.
 *
 * The recursion guard (APPIMAGE_SCOPED=1 + stripping our own lib from
 * LD_PRELOAD when spawning systemd-run) prevents the hook from looping.
 * Any failure falls back to the real execve.
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

typedef int (*execve_t)(const char *, char *const[], char *const[]);
typedef int (*execveat_t)(int, const char *, char *const[], char *const[], int);

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
        out[o++] = envp[i];
    }
    out[o++] = "APPIMAGE_SCOPED=1";
    out[o] = NULL;
    return o;
}

static int redirect_exec(const char *path, char *const argv[], char *const envp[]) {
    static execve_t real_execve = NULL;
    if (!real_execve) real_execve = (execve_t)dlsym(RTLD_NEXT, "execve");
    if (!real_execve) { errno = ENOSYS; return -1; }

    int argc = count_argv(argv);
    int envc = count_envp(envp);

    char **newargv = calloc(argc + 8, sizeof(char *));
    char **newenvp = calloc(envc + 8, sizeof(char *));
    if (!newargv || !newenvp) { free(newargv); free(newenvp); errno = ENOMEM; return -1; }

    int i = 0;
    newargv[i++] = "systemd-run";
    newargv[i++] = "--user";
    newargv[i++] = "--scope";
    newargv[i++] = "--slice=app.slice";
    newargv[i++] = "--";
    newargv[i++] = (char *)path;
    for (int j = 1; j < argc; j++) newargv[i++] = argv[j];
    newargv[i] = NULL;

    int nenv = scrub_envp(newenvp, envp);

    real_execve("/usr/bin/systemd-run", newargv, newenvp);
    /* exec failed; fall through and let caller run the original */
    int saved = errno;
    for (int j = 0; j < nenv; j++) free(newenvp[j]);
    free(newargv); free(newenvp);
    errno = saved;
    return -1;
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    static execve_t real = NULL;
    if (!real) real = (execve_t)dlsym(RTLD_NEXT, "execve");
    if (!real) { errno = ENOSYS; return -1; }
    if (getenv("APPIMAGE_SCOPED")) return real(path, argv, envp);
    if (is_appimage(path) || is_sas(path)) {
        redirect_exec(path, argv, envp);
    }
    return real(path, argv, envp);
}

int execveat(int dirfd, const char *path, char *const argv[], char *const envp[], int flags) {
    static execveat_t real = NULL;
    if (!real) real = (execveat_t)dlsym(RTLD_NEXT, "execveat");
    if (!real) { errno = ENOSYS; return -1; }
    if (getenv("APPIMAGE_SCOPED")) return real(dirfd, path, argv, envp, flags);
    if (dirfd == AT_FDCWD && (is_appimage(path) || is_sas(path))) {
        redirect_exec(path, argv, envp);
    }
    return real(dirfd, path, argv, envp, flags);
}
