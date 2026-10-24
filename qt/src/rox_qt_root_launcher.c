/*
 * ROX-Filer2-QT fixed root launcher.
 *
 * 2.13.0-9: launched only through pkexec from the normal-user context menu.
 * It accepts exactly one existing directory, resolves it with realpath() and
 * starts the fixed package-owned Rox-Filer2 binary.  It never invokes a shell
 * and never accepts an executable path from the caller.
 *
 * Theme inheritance: pkexec scrubs the caller's environment and HOME becomes
 * /root, so the elevated Qt used to fall back to a generic style.  The
 * unprivileged wrapper (roxfiler-qt-root) now forwards a short list of
 * theme-related variables as --env=NAME=VALUE arguments.  Every one of them
 * is validated here against a fixed whitelist; variables that can load code
 * (LD_*, QT_QPA_PLATFORM_PLUGIN_PATH ...) are never accepted, and path lists
 * are only honoured for root-owned, non-writable system directories.  The
 * Only allowlisted KDE theme keys from ~/.config/kdeglobals are copied to a
 * private root-owned XDG_CONFIG_HOME. Never expose the complete user config
 * tree to elevated Qt; root never writes into the user's home.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define ROX_FILER_BINARY "/usr/bin/rox-filer2-qt"

static void usage(const char *argv0)
{
    fprintf(stderr, "Usage: %s [--env=NAME=VALUE]... [DIRECTORY]\n", argv0);
}

static bool validate_directory(const char *input, char resolved[PATH_MAX])
{
    struct stat st;

    if (!input || input[0] != '/') {
        fprintf(stderr, "Directory must be an absolute path.\n");
        return false;
    }
    if (!realpath(input, resolved)) {
        fprintf(stderr, "Could not resolve '%s': %s\n", input, strerror(errno));
        return false;
    }
    if (stat(resolved, &st) != 0) {
        fprintf(stderr, "Could not stat '%s': %s\n", resolved, strerror(errno));
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Not a directory: %s\n", resolved);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------ environment */

enum env_kind { ENV_WORD, ENV_NUMBER, ENV_SYSTEM_PATHS, ENV_CONFIG_DIRS, ENV_PALETTE, ENV_FONT };

static const struct {
    const char *name;
    enum env_kind kind;
} allowed_env[] = {
    { "ROX_QT_THEME_PALETTE",     ENV_PALETTE },
    { "ROX_QT_THEME_ICON",        ENV_WORD },
    { "ROX_QT_THEME_FONT",        ENV_FONT },
    { "LANG",                         ENV_WORD },
    { "LANGUAGE",                     ENV_WORD },
    { "LC_MESSAGES",                  ENV_WORD },
    { "LC_ALL",                       ENV_WORD },
    { "QT_QPA_PLATFORMTHEME",        ENV_WORD },
    { "QT_STYLE_OVERRIDE",           ENV_WORD },
    { "QT_QUICK_CONTROLS_STYLE",     ENV_WORD },
    { "XDG_CURRENT_DESKTOP",         ENV_WORD },
    { "XDG_SESSION_DESKTOP",         ENV_WORD },
    { "DESKTOP_SESSION",             ENV_WORD },
    { "KDE_FULL_SESSION",            ENV_WORD },
    { "KDE_SESSION_VERSION",         ENV_NUMBER },
    { "QT_SCALE_FACTOR",             ENV_NUMBER },
    { "QT_FONT_DPI",                 ENV_NUMBER },
    { "QT_AUTO_SCREEN_SCALE_FACTOR", ENV_NUMBER },
    { "QT_ENABLE_HIGHDPI_SCALING",   ENV_NUMBER },
    { "QT_PLUGIN_PATH",              ENV_SYSTEM_PATHS },
    { "XDG_DATA_DIRS",               ENV_SYSTEM_PATHS },
    { "XDG_CONFIG_DIRS",             ENV_CONFIG_DIRS },
};

#define MAX_ENV_REQUESTS 32
#define MAX_ENV_LENGTH   4096

struct env_request {
    char *name;
    char *value;
};

static struct env_request env_requests[MAX_ENV_REQUESTS];
static size_t env_request_count;

static struct passwd *caller_passwd(void)
{
    const char *uid_text = getenv("PKEXEC_UID");
    char *end = NULL;
    unsigned long raw_uid;

    if (!uid_text || !*uid_text)
        return NULL;
    errno = 0;
    raw_uid = strtoul(uid_text, &end, 10);
    if (errno || !end || *end != '\0' || raw_uid > (unsigned long)((uid_t)-1))
        return NULL;
    return getpwuid((uid_t)raw_uid);
}

static bool word_is_safe(const char *value, size_t max)
{
    size_t i, len = strlen(value);

    if (len == 0 || len > max)
        return false;
    for (i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (!(isalnum(c) || c == '_' || c == '-' || c == '.' || c == ':' ||
              c == ',' || c == ';' || c == '+'))
            return false;
    }
    return true;
}

/* Exactly 60 ARGB hex colours, no paths, markup or executable content. */
static bool palette_is_safe(const char *value)
{
    size_t i, len = strlen(value);
    if (len != 60 * 8 + 59) return false;
    for (i = 0; i < len; ++i) {
        if ((i + 1) % 9 == 0) {
            if (value[i] != ',') return false;
        } else if (!isxdigit((unsigned char)value[i])) return false;
    }
    return true;
}

static bool font_is_safe(const char *value)
{
    size_t i, len = strlen(value);
    if (!len || len > 256) return false;
    for (i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (!(isalnum(c) || c == ' ' || c == ',' || c == '_' ||
              c == '-' || c == '.' || c == '+')) return false;
    }
    return true;
}

static bool number_is_safe(const char *value)
{
    size_t i, len = strlen(value);

    if (len == 0 || len > 16)
        return false;
    for (i = 0; i < len; ++i) {
        if (!isdigit((unsigned char)value[i]) && value[i] != '.')
            return false;
    }
    return true;
}

/* Every component of the (already resolved) path must be owned by root and
 * not writable by group/others, so an unprivileged user cannot swap it. */
static bool chain_is_root_owned(const char *resolved)
{
    char buf[PATH_MAX];
    struct stat st;
    char *slash;

    if (strlen(resolved) >= sizeof(buf))
        return false;
    strcpy(buf, resolved);
    for (;;) {
        if (stat(buf, &st) != 0 || st.st_uid != 0 || (st.st_mode & (S_IWGRP | S_IWOTH)))
            return false;
        if (strcmp(buf, "/") == 0)
            return true;
        slash = strrchr(buf, '/');
        if (!slash)
            return false;
        if (slash == buf)
            buf[1] = '\0';
        else
            *slash = '\0';
    }
}

static bool system_dir_is_trusted(const char *path, char resolved[PATH_MAX])
{
    struct stat st;

    if (!path || path[0] != '/' || !realpath(path, resolved))
        return false;
    if (strncmp(resolved, "/usr/", 5) != 0 && strncmp(resolved, "/opt/essora/", 12) != 0 &&
        strncmp(resolved, "/etc/", 5) != 0)
        return false;
    if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode))
        return false;
    return chain_is_root_owned(resolved);
}

/* Never add the caller's full ~/.config to a privileged Qt process's
 * XDG_CONFIG_DIRS: it may contain arbitrary plugin and platform configuration.
 * A restricted kdeglobals palette/theme snapshot is created below instead. */

/* Filter a ':' separated list, keeping only acceptable entries.  Returns a
 * malloc'ed string (possibly empty) or NULL when nothing is left. */
static char *filter_path_list(const char *value)
{
    char *copy, *save = NULL, *entry, *out;
    char resolved[PATH_MAX];
    size_t used = 0;

    if (strlen(value) > MAX_ENV_LENGTH)
        return NULL;
    copy = strdup(value);
    out = calloc(1, MAX_ENV_LENGTH + PATH_MAX + 2);
    if (!copy || !out) {
        free(copy);
        free(out);
        return NULL;
    }
    for (entry = strtok_r(copy, ":", &save); entry; entry = strtok_r(NULL, ":", &save)) {
        size_t len;
        if (!system_dir_is_trusted(entry, resolved))
            continue;
        len = strlen(resolved);
        if (used + len + 2 > MAX_ENV_LENGTH)
            break;
        if (used)
            out[used++] = ':';
        memcpy(out + used, resolved, len);
        used += len;
        out[used] = '\0';
    }
    free(copy);
    if (!used) {
        free(out);
        return NULL;
    }
    return out;
}

static bool register_env_request(const char *assignment)
{
    const char *eq = strchr(assignment, '=');
    size_t i, name_len;
    char name[64];

    if (!eq || eq == assignment)
        return false;
    name_len = (size_t)(eq - assignment);
    if (name_len >= sizeof(name))
        return false;
    memcpy(name, assignment, name_len);
    name[name_len] = '\0';
    eq++;

    for (i = 0; i < sizeof(allowed_env) / sizeof(allowed_env[0]); ++i) {
        char *value = NULL;
        if (strcmp(allowed_env[i].name, name) != 0)
            continue;
        switch (allowed_env[i].kind) {
        case ENV_WORD:
            if (word_is_safe(eq, 96))
                value = strdup(eq);
            break;
        case ENV_NUMBER:
            if (number_is_safe(eq))
                value = strdup(eq);
            break;
        case ENV_SYSTEM_PATHS:
            value = filter_path_list(eq);
            break;
        case ENV_CONFIG_DIRS:
            value = filter_path_list(eq);
            break;
        case ENV_PALETTE:
            if (palette_is_safe(eq)) value = strdup(eq);
            break;
        case ENV_FONT:
            if (font_is_safe(eq)) value = strdup(eq);
            break;
        }
        if (!value) {
            fprintf(stderr, "Ignoring unsafe or unusable value for %s.\n", name);
            return false;
        }
        if (env_request_count >= MAX_ENV_REQUESTS) {
            free(value);
            return false;
        }
        env_requests[env_request_count].name = strdup(name);
        env_requests[env_request_count].value = value;
        env_request_count++;
        return true;
    }
    fprintf(stderr, "Ignoring environment variable that is not allowed: %s\n", name);
    return false;
}

static const char *requested_env(const char *name)
{
    size_t i;

    for (i = 0; i < env_request_count; ++i) {
        if (strcmp(env_requests[i].name, name) == 0)
            return env_requests[i].value;
    }
    return NULL;
}

/* Filter the user's KDE colour/icon/style preferences through a strict
 * allowlist; do not import scripts, plugin paths or user-owned style files.
 * Qt's platform theme reads kdeglobals from the private XDG_CONFIG_HOME.
 * The output lives under /root, and is always replaced atomically.
 */
static bool theme_section(const char *name)
{
    static const char *sections[] = {
        "General", "KDE", "Icons", "Colors:Window", "Colors:View",
        "Colors:Button", "Colors:Selection", "Colors:Tooltip",
        "Colors:Complementary", "Colors:Header"
    };
    size_t i;
    for (i = 0; i < sizeof(sections) / sizeof(sections[0]); i++)
        if (strcmp(name, sections[i]) == 0) return true;
    return false;
}

static bool theme_value_ok(const char *section, const char *key, const char *value)
{
    size_t i, n = strlen(value);
    if (!n || n > 180) return false;
    if (strncmp(section, "Colors:", 7) == 0) {
        /* Only literal colours are used, never instructions to open files. */
        if (strncmp(key, "Background", 10) != 0 &&
            strncmp(key, "Foreground", 10) != 0 &&
            strncmp(key, "Decoration", 10) != 0) return false;
        for (i = 0; i < n; ++i)
            if (!(isxdigit((unsigned char)value[i]) || isdigit((unsigned char)value[i]) ||
                  value[i] == ',' || value[i] == '#' || value[i] == ' ')) return false;
        return true;
    }
    if (strcmp(section, "Icons") == 0)
        return strcmp(key, "Theme") == 0 && word_is_safe(value, 96);
    if (strcmp(section, "KDE") == 0)
        return strcmp(key, "widgetStyle") == 0 && word_is_safe(value, 96);
    if (strcmp(section, "General") == 0) {
        if (strcmp(key, "ColorScheme") == 0)
            return word_is_safe(value, 96);
        if (strcmp(key, "font") != 0 && strcmp(key, "fixed") != 0 &&
            strcmp(key, "menuFont") != 0 && strcmp(key, "toolBarFont") != 0 &&
            strcmp(key, "smallestReadableFont") != 0) return false;
        for (i = 0; i < n; ++i)
            if (!isprint((unsigned char)value[i]) || value[i] == '[' ||
                value[i] == ']' || value[i] == ';' || value[i] == '=') return false;
        return true;
    }
    return false;
}

static void copy_safe_kdeglobals(const char *source_config, const char *destination_config,
                                 uid_t caller_uid)
{
    char input[PATH_MAX], output[PATH_MAX], temporary[PATH_MAX], line[1024];
    char current_section[64] = "";
    struct stat st;
    int source_fd, dest_fd;
    FILE *src, *dst;
    if (snprintf(input, sizeof(input), "%s/kdeglobals", source_config) >= (int)sizeof(input) ||
        snprintf(output, sizeof(output), "%s/kdeglobals", destination_config) >= (int)sizeof(output) ||
        snprintf(temporary, sizeof(temporary), "%s/kdeglobals.XXXXXX", destination_config) >= (int)sizeof(temporary))
        return;
    source_fd = open(input, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (source_fd < 0) return;
    if (fstat(source_fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != caller_uid || st.st_size > 262144 || st.st_size < 0) {
        close(source_fd);
        return;
    }
    src = fdopen(source_fd, "r");
    if (!src) { close(source_fd); return; }
    dest_fd = mkstemp(temporary);
    if (dest_fd < 0) { fclose(src); return; }
    (void)fchmod(dest_fd, 0600);
    dst = fdopen(dest_fd, "w");
    if (!dst) { close(dest_fd); unlink(temporary); fclose(src); return; }
    while (fgets(line, sizeof(line), src)) {
        char *key, *value, *eq, *end;
        size_t len = strlen(line);
        if (!len || (line[len - 1] != '\n' && !feof(src))) {
            /* Skip overlong lines completely. */
            int c;
            while ((c = fgetc(src)) != '\n' && c != EOF) {}
            continue;
        }
        while (len && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        if (line[0] == '[') {
            end = strchr(line, ']');
            current_section[0] = '\0';
            if (!end || end[1] != '\0') continue;
            *end = '\0';
            if (strlen(line + 1) < sizeof(current_section) && theme_section(line + 1)) {
                strcpy(current_section, line + 1);
                fprintf(dst, "[%s]\n", current_section);
            }
            continue;
        }
        if (!current_section[0]) continue;
        eq = strchr(line, '=');
        if (!eq || eq == line) continue;
        *eq = '\0'; key = line; value = eq + 1;
        if (strlen(key) > 50) continue;
        for (end = key; *end; ++end)
            if (!(isalnum((unsigned char)*end) || *end == '_')) break;
        if (*end || !theme_value_ok(current_section, key, value)) continue;
        fprintf(dst, "%s=%s\n", key, value);
    }
    if (ferror(src) || fflush(dst) != 0 || fsync(dest_fd) != 0) {
        fclose(src); fclose(dst); unlink(temporary); return;
    }
    fclose(src);
    if (fclose(dst) != 0 || rename(temporary, output) != 0) unlink(temporary);
}

/* Root keeps its own private XDG_CONFIG_HOME. Copy only selected theme keys,
 * not the caller's complete ~/.config, and preserve system-owned XDG layers. */
static void prepare_theme_configuration(void)
{
    /* getpwuid() returns a static buffer: copy every string we need before
     * the next call, otherwise the second lookup overwrites the first. */
    char root_home[PATH_MAX], caller_home[PATH_MAX];
    char caller_config[PATH_MAX], real_caller_config[PATH_MAX];
    char root_config[PATH_MAX], private_config[PATH_MAX], link_path[PATH_MAX], link_target[PATH_MAX];
    const char *requested_dirs;
    uid_t caller_uid;
    struct passwd *pw;
    struct stat st;

    pw = getpwuid(0);
    if (!pw || !pw->pw_dir || !*pw->pw_dir ||
        snprintf(root_home, sizeof(root_home), "%s", pw->pw_dir) >= (int)sizeof(root_home))
        return;
    pw = caller_passwd();
    if (!pw || !pw->pw_dir || !*pw->pw_dir || pw->pw_uid == 0 ||
        snprintf(caller_home, sizeof(caller_home), "%s", pw->pw_dir) >= (int)sizeof(caller_home))
        return;
    if (strcmp(root_home, caller_home) == 0)
        return;
    caller_uid = pw->pw_uid;

    if (snprintf(caller_config, sizeof(caller_config), "%s/.config", caller_home) >=
            (int)sizeof(caller_config) ||
        !realpath(caller_config, real_caller_config) ||
        stat(real_caller_config, &st) != 0 || !S_ISDIR(st.st_mode))
        return;

    if (snprintf(root_config, sizeof(root_config), "%s/.config", root_home) >=
            (int)sizeof(root_config) ||
        snprintf(private_config, sizeof(private_config), "%s/rox-filer2-qt-root", root_config) >=
            (int)sizeof(private_config))
        return;
    if (mkdir(root_config, 0700) != 0 && errno != EEXIST)
        return;
    if (mkdir(private_config, 0700) != 0 && errno != EEXIST)
        return;
    if (lstat(private_config, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0 ||
        (st.st_mode & (S_IWGRP | S_IWOTH)))
        return;

    /* Keep root's own ROX customisations (templates, MIME icons) reachable. */
    if (snprintf(link_path, sizeof(link_path), "%s/rox.sourceforge.net", private_config) <
            (int)sizeof(link_path) &&
        snprintf(link_target, sizeof(link_target), "%s/rox.sourceforge.net", root_config) <
            (int)sizeof(link_target))
        (void)symlink(link_target, link_path);

    copy_safe_kdeglobals(real_caller_config, private_config, caller_uid);
    requested_dirs = requested_env("XDG_CONFIG_DIRS");
    (void)setenv("XDG_CONFIG_HOME", private_config, 1);
    (void)setenv("XDG_CONFIG_DIRS", requested_dirs ? requested_dirs : "/etc/xdg", 1);
}

static void apply_requested_environment(void)
{
    size_t i;

    for (i = 0; i < env_request_count; ++i) {
        if (strcmp(env_requests[i].name, "XDG_CONFIG_DIRS") == 0)
            continue; /* merged by prepare_theme_configuration() */
        (void)setenv(env_requests[i].name, env_requests[i].value, 1);
    }
    prepare_theme_configuration();
}

static void preserve_caller_xauthority(void)
{
    const char *current = getenv("XAUTHORITY");
    struct passwd *caller;
    char candidate[PATH_MAX];

    /* pkexec with allow_gui=true preserves XAUTHORITY when the session sets
     * it explicitly. Traditional X11 sessions often leave it unset and rely
     * on ~/.Xauthority, so derive that path from PKEXEC_UID before HOME is
     * switched to /root. */
    if (current && *current)
        return;

    caller = caller_passwd();
    if (!caller || !caller->pw_dir || !*caller->pw_dir)
        return;
    if (snprintf(candidate, sizeof(candidate), "%s/.Xauthority", caller->pw_dir) >=
            (int)sizeof(candidate))
        return;
    if (access(candidate, R_OK) == 0)
        (void)setenv("XAUTHORITY", candidate, 1);
}

static void prepare_root_environment(void)
{
    struct passwd *pw;

    preserve_caller_xauthority();
    pw = getpwuid(0);

    if (pw && pw->pw_dir && *pw->pw_dir)
        (void)setenv("HOME", pw->pw_dir, 1);
    (void)setenv("USER", "root", 1);
    (void)setenv("LOGNAME", "root", 1);

    /* Elevated Qt runs on X11/XLibre or XWayland via xcb; not on the
     * user's private Wayland socket. Never change access via xhost. */
    (void)setenv("QT_QPA_PLATFORM", "xcb", 1);
    (void)unsetenv("WAYLAND_DISPLAY");
    (void)unsetenv("XDG_RUNTIME_DIR");

    /* Theme variables forwarded (and validated) from the unprivileged caller. */
    apply_requested_environment();
}

int main(int argc, char **argv)
{
    char resolved[PATH_MAX];
    const char *directory = NULL;
    bool options_done = false;
    pid_t pid;
    int i;

    if (geteuid() != 0) {
        fprintf(stderr, "rox-qt-root-launcher must be run as root through pkexec.\n");
        return 1;
    }
    for (i = 1; i < argc; ++i) {
        if (!options_done && strcmp(argv[i], "--") == 0) {
            options_done = true;
        } else if (!options_done && strncmp(argv[i], "--env=", 6) == 0) {
            (void)register_env_request(argv[i] + 6);
        } else if (!directory) {
            directory = argv[i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!getenv("DISPLAY") || !*getenv("DISPLAY")) {
        fprintf(stderr, "An X11/XLibre or XWayland DISPLAY is required.\n");
        return 1;
    }
    if (access(ROX_FILER_BINARY, X_OK) != 0) {
        fprintf(stderr, "Required program '%s' was not found or is not executable.\n",
                ROX_FILER_BINARY);
        return 1;
    }
    if (!validate_directory(directory ? directory : "/", resolved))
        return 1;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed: %s\n", strerror(errno));
        return 1;
    }
    if (pid > 0)
        return 0;

    (void)setsid();
    prepare_root_environment();
    if (chdir(resolved) != 0) {
        fprintf(stderr, "Could not enter '%s': %s\n", resolved, strerror(errno));
        _exit(1);
    }

    execl(ROX_FILER_BINARY, "rox-filer2-qt", resolved, (char *)NULL);
    fprintf(stderr, "Could not start '%s': %s\n", ROX_FILER_BINARY, strerror(errno));
    _exit(1);
}
