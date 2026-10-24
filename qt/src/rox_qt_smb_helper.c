/*
 * ROX-Filer2-QT native SMB mount helper (without GIO / GVfs).
 * Started by Polkit only. Credentials are received on standard input, never
 * through argv, and written to a short-lived root-only credentials file.
 * No shell. Fixed mount prefix, restricted UNC components and mount options.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mntent.h>
#include <sys/mount.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static bool name_ok(const char *s, size_t max)
{
    size_t n;
    if (!s || !*s || !strcmp(s, ".") || !strcmp(s, "..")) return false;
    n = strlen(s);
    if (n > max) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (!(isalnum(c) || c == '-' || c == '_' || c == '.')) return false;
    }
    return true;
}

/* Work exclusively under root-owned, non-symlink directories. */
static bool root_directory(const char *dir, mode_t mode)
{
    struct stat st;
    if (mkdir(dir, mode) < 0 && errno != EEXIST) return false;
    if (lstat(dir, &st) < 0) return false;
    return S_ISDIR(st.st_mode) && st.st_uid == 0 && !(st.st_mode & 0022);
}

static bool read_line(char *out, size_t n)
{
    size_t len;
    if (!fgets(out, (int)n, stdin)) return false;
    len = strlen(out);
    if (len == 0 || out[len - 1] != '\n') return false;
    out[len - 1] = 0;
    return true;
}

static bool share_is_mounted(const char *point)
{
    FILE *f = setmntent("/proc/self/mounts", "r");
    if (!f) return false;
    struct mntent *ent;
    bool mounted = false;
    while ((ent = getmntent(f))) {
        if (!strcmp(ent->mnt_dir, point) && !strcmp(ent->mnt_type, "cifs")) {
            mounted = true;
            break;
        }
    }
    endmntent(f);
    return mounted;
}

static int run(char *const argv[])
{
    pid_t child = fork();
    int status;
    if (child == -1) return -1;
    if (child == 0) {
        execv(argv[0], argv);
        _exit(127);
    }
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int main(int argc, char **argv)
{
    char *end = NULL;
    const char *pk_uid = getenv("PKEXEC_UID");
    unsigned long nuid;
    struct passwd *pw;
    char host[128], share[128], user[256], password[1026];
    char mountbase[PATH_MAX], mountdir[PATH_MAX], mounttarget[PATH_MAX];
    char creds[] = "/run/rox-filer2-qt-smb-XXXXXX";
    char options[PATH_MAX+150];
    char *mountcifs;
    int fd = -1;
    int result;
    bool made = false;
    ssize_t written;
    char data[1500];

    if (geteuid() != 0 || (argc != 4 && argc != 3)) {
        fprintf(stderr, "Usage: rox-qt-smb-helper HOST SHARE [USER] (password on stdin).\n");
        return 2;
    }
    errno = 0;
    nuid = (pk_uid && *pk_uid) ? strtoul(pk_uid, &end, 10) : 0;
    if (errno || (pk_uid && *pk_uid && (!end || *end)) || nuid > UINT_MAX ||
        !(pw = getpwuid((uid_t)nuid))) {
        fprintf(stderr, "Invalid authenticated user.\n");
        return 2;
    }
    if (!name_ok(argv[1], 100) || !name_ok(argv[2], 100) ||
        snprintf(host, sizeof(host), "%s", argv[1]) >= (int)sizeof(host) ||
        snprintf(share, sizeof(share), "%s", argv[2]) >= (int)sizeof(share)) {
        fprintf(stderr, "Unsupported server/share name. Use alphanumeric, dash, underscore and dot.\n");
        return 2;
    }
    if (argc == 4 && (strlen(argv[3]) >= sizeof(user) || strchr(argv[3], '\n') || strchr(argv[3], '\r'))) {
        fprintf(stderr, "Invalid SMB username.\n");
        return 2;
    }
    if (argc == 4) {
        strcpy(user, *argv[3] ? argv[3] : "guest");
        if (!read_line(password, sizeof(password))) {
            fprintf(stderr, "Password input was not received.\n");
            return 2;
        }
        if (strchr(password, '\r')) {
            fprintf(stderr, "Invalid SMB password.\n");
            return 2;
        }
    }
    if (!root_directory("/mnt", 0755) ||
        !root_directory("/mnt/rox-filer2-qt-smb", 0755)) {
        fprintf(stderr, "Unsafe SMB mounting directory.\n");
        return 1;
    }
    if (snprintf(mountbase, sizeof(mountbase), "/mnt/rox-filer2-qt-smb/%lu", nuid) >= (int)sizeof(mountbase) ||
        !root_directory(mountbase, 0755) ||
        snprintf(mountdir, sizeof(mountdir), "%s/%zu_%s_%s", mountbase, strlen(host), host, share) >= (int)sizeof(mountdir)) {
        fprintf(stderr, "Unsafe SMB mount location.\n");
        return 1;
    }
    if (share_is_mounted(mountdir)) {
        if (argc == 3) {
            if (umount2(mountdir, 0) != 0) {
                perror("umount");
                return 1;
            }
            (void)rmdir(mountdir);
        }
        puts(mountdir);
        return 0;
    }
    if (argc == 3) {
        fprintf(stderr, "This SMB share is not mounted here.\n");
        return 1;
    }
    {
        struct stat st;
        if (lstat(mountdir, &st) < 0 && errno == ENOENT) made = true;
        if (!root_directory(mountdir, 0755)) {
            fprintf(stderr, "Unsafe SMB mount target.\n");
            return 1;
        }
    }
    if (access("/usr/sbin/mount.cifs", X_OK) == 0) mountcifs = "/usr/sbin/mount.cifs";
    else if (access("/sbin/mount.cifs", X_OK) == 0) mountcifs = "/sbin/mount.cifs";
    else {
        fprintf(stderr, "mount.cifs is missing (install cifs-utils).\n");
        return 127;
    }
    fd = mkstemp(creds);
    if (fd < 0) {
        perror("mkstemp");
        return 1;
    }
    (void)fchmod(fd, 0600);
    /* Neither username nor password can include a newline. */
    result = snprintf(data, sizeof(data), "username=%s\npassword=%s\n", user, password);
    explicit_bzero(password, sizeof(password));
    if (result < 0 || result >= (int)sizeof(data)) {
        close(fd); unlink(creds); return 2;
    }
    written = write(fd, data, (size_t)result);
    explicit_bzero(data, sizeof(data));
    if (written != result || close(fd) < 0) {
        unlink(creds);
        return 1;
    }
    if (snprintf(mounttarget, sizeof(mounttarget), "//%s/%s", host, share) >= (int)sizeof(mounttarget) ||
        snprintf(options, sizeof(options), "credentials=%s,uid=%lu,gid=%lu,file_mode=0600,dir_mode=0700,nosuid,nodev",
                 creds, nuid, (unsigned long)pw->pw_gid) >= (int)sizeof(options)) {
        unlink(creds);
        return 1;
    }
    {
        char *const args[] = {mountcifs, mounttarget, mountdir, "-o", options, NULL};
        result = run(args);
    }
    unlink(creds);
    if (result == 0) {
        puts(mountdir);
        return 0;
    }
    if (made) (void)rmdir(mountdir);
    fprintf(stderr, "Could not mount SMB share. Check credentials, server and cifs-utils.\n");
    return 1;
}
