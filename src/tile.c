/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The icon on the home screen. Follows ps5upload's register.c and webui_tile.c:
 * a folder holding sce_sys and a placeholder eboot.bin, a copy of sce_sys under
 * /user/app and /user/appmeta, the folder bound over /system_ex/app/<id>, and Sony's
 * installer told about it. Opening the icon starts the console's browser at the
 * deeplink, 127.0.0.1:TILE_PORT, which local_http.c answers.
 *
 * No mount.lnk is written: that file is what other tools take for a game of theirs.
 *
 * The first registration runs only from the page's button, and each step goes to the log
 * first. The bind is lost at every reboot; the next run binds it again and leaves Sony's
 * installer alone, which it knows from a mark written only after the installer agreed.
 * Files alone prove nothing: a run that stopped half-way leaves them too.
 * Deleting the files alone would leave a grey icon, so removing goes through Sony.
 *
 * libSceIpmi and libSceAppInstUtil are in this ELF's DT_NEEDED, in that order, as in
 * ps5upload: upstream says the order matters (ps5-payload-dev/sdk#26), and a
 * library opened with dlopen() is not searched by dlsym(RTLD_DEFAULT). */
#include "tile.h"
#include "http_server.h"
#include "crash.h"
#include "log.h"

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include <ps5/kernel.h>

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TITLE TILE_TITLE_ID
#define TILE_DIR APP_DIR "/tile/" TITLE
#define REGISTERED_MARK APP_DIR "/tile/registered-" TITLE
#define USER_APP "/user/app/" TITLE
#define APPMETA "/user/appmeta/" TITLE
#define MOUNT_POINT "/system_ex/app/" TITLE
#define APPINST_ALREADY 0x80990002u
#define SHELLCORE_AUTHID 0x3800000000000010ULL
#define SYSTEM_AUTHID 0x4800000000000006ULL
#define SONY_SETTLE_US 200000
#define MAX_PATH_BYTES 512
#define TILE_STACK (1024 * 1024)

typedef int (*init_fn)(void);
typedef int (*install_dir_fn)(const char *, const char *, int);
typedef int (*install_all_fn)(void *);
typedef int (*uninstall_fn)(const char *, void *, void *);

static const char g_eboot[] = APP_NAME " home-screen icon: opens the browser on the console's page\n";

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_state[24] = "idle", g_error[32];
static int g_elevated;

static void set_state(const char *state, const char *error)
{
    pthread_mutex_lock(&g_lock);
    snprintf(g_state, sizeof g_state, "%s", state);
    snprintf(g_error, sizeof g_error, "%s", error);
    pthread_mutex_unlock(&g_lock);
}

void tile_status(char *out, size_t capacity)
{
    pthread_mutex_lock(&g_lock);
    snprintf(out, capacity, "{\"state\":\"%s\",\"error\":\"%s\"}", g_state, g_error);
    pthread_mutex_unlock(&g_lock);
}

int tile_elevate(void)
{
    static const uint8_t caps[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    static const uint8_t attrs[32] = {0, 0, 0, 0x80};
    intptr_t root;
    int rc = 0;

    rc |= kernel_set_ucred_uid(-1, 0);
    rc |= kernel_set_ucred_ruid(-1, 0);
    rc |= kernel_set_ucred_svuid(-1, 0);
    rc |= kernel_set_ucred_rgid(-1, 0);
    rc |= kernel_set_ucred_svgid(-1, 0);
    rc |= kernel_set_ucred_caps(-1, caps);
    rc |= kernel_set_ucred_attrs(-1, attrs);
    rc |= kernel_set_ucred_authid(-1, SYSTEM_AUTHID);
    root = kernel_get_root_vnode();
    if (root) {
        rc |= kernel_set_proc_rootdir(-1, root);
        rc |= kernel_set_proc_jaildir(-1, root);
    } else {
        rc = -1;
    }
    g_elevated = !rc;
    say("elevated: rc %d, uid %d, authid %#llx", rc, (int)getuid(), (unsigned long long)kernel_get_ucred_authid(getpid()));
    return rc ? -1 : 0;
}

/* ---- the files ------------------------------------------------------------ */

static int make_parents(const char *dir)
{
    char path[MAX_PATH_BYTES];
    size_t i, length = strlen(dir);

    if (length >= sizeof path) return -1;
    memcpy(path, dir, length + 1);
    for (i = 1; i <= length; i++) {
        if (path[i] != '/' && path[i]) continue;
        path[i] = 0;
        if (mkdir(path, 0777) != 0 && errno != EEXIST) return -1;
        path[i] = i < length ? '/' : 0;
    }
    return 0;
}

static int write_file(const char *path, const void *data, size_t length)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666), ok;

    if (fd < 0) return -1;
    ok = write(fd, data, length) == (ssize_t)length;
    close(fd);
    return ok ? 0 : -1;
}

static int write_in(const char *dir, const char *name, const void *data, size_t length)
{
    char path[MAX_PATH_BYTES];

    snprintf(path, sizeof path, "%s/%s", dir, name);
    return write_file(path, data, length);
}

static int write_sce_sys(const char *dir, const char *param, size_t param_length)
{
    if (make_parents(dir) != 0) return -1;
    if (write_in(dir, "param.json", param, param_length) != 0) return -1;
    return write_in(dir, "icon0.png", app_icon, app_icon_length);
}

static int write_files(void)
{
    char param[512];
    int length = snprintf(param, sizeof param,
        "{\"applicationCategoryType\":66048,\"titleId\":\"" TITLE "\",\"deeplinkUri\":\"http://127.0.0.1:%d/\","
        "\"localizedParameters\":{\"defaultLanguage\":\"en-US\",\"en-US\":{\"titleName\":\"" APP_NAME "\"}}}", TILE_PORT);

    if (write_sce_sys(TILE_DIR "/sce_sys", param, (size_t)length) != 0) return -1;
    if (write_in(TILE_DIR, "eboot.bin", g_eboot, sizeof g_eboot - 1) != 0) return -1;
    if (write_sce_sys(USER_APP "/sce_sys", param, (size_t)length) != 0) return -1;
    if (write_in(USER_APP, "icon0.png", app_icon, app_icon_length) != 0) return -1;
    if (make_parents(APPMETA) != 0) return -1;
    if (write_in(APPMETA, "param.json", param, (size_t)length) != 0) return -1;
    return write_in(APPMETA, "icon0.png", app_icon, app_icon_length);
}

static void remove_tree(const char *path, int depth)
{
    struct stat st;
    struct dirent *e;
    DIR *d;

    if (lstat(path, &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) {
        unlink(path);
        return;
    }
    if (depth > 4) return;
    d = opendir(path);
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        char child[MAX_PATH_BYTES];

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        remove_tree(child, depth + 1);
    }
    closedir(d);
    rmdir(path);
}

/* ---- the bind -------------------------------------------------------------- */

static int bound(void)
{
    struct statfs fs;

    return statfs(MOUNT_POINT, &fs) == 0 && !strcmp(fs.f_mntonname, MOUNT_POINT);
}

/* FW 13 mounts /system_ex read-only, though it was not seen so on this console. */
static int remount_writable(void)
{
    struct statfs fs;
    struct iovec iov[6];
    char fstype[] = "fstype", fspath[] = "fspath", from[] = "from", mount_at[] = "/system_ex";

    if (statfs(mount_at, &fs) != 0 || strcmp(fs.f_mntonname, mount_at)) return -1;
    iov[0] = (struct iovec){fstype, sizeof fstype};
    iov[1] = (struct iovec){fs.f_fstypename, strlen(fs.f_fstypename) + 1};
    iov[2] = (struct iovec){fspath, sizeof fspath};
    iov[3] = (struct iovec){mount_at, sizeof mount_at};
    iov[4] = (struct iovec){from, sizeof from};
    iov[5] = (struct iovec){fs.f_mntfromname, strlen(fs.f_mntfromname) + 1};
    return nmount(iov, 6, MNT_UPDATE);
}

static const char *bind_folder(void)
{
    char fstype[] = "fstype", nullfs[] = "nullfs", from[] = "from", fspath[] = "fspath", source[] = TILE_DIR, target[] = MOUNT_POINT;
    struct iovec iov[6];
    int made;

    if (make_parents("/system_ex/app") != 0 && errno != EROFS) return "mkdir_app";
    made = mkdir(MOUNT_POINT, 0755);
    if (made != 0 && errno == EROFS) {
        say("tile: /system_ex is read-only, remount %d", remount_writable());
        made = mkdir(MOUNT_POINT, 0755);
    }
    if (made != 0 && errno != EEXIST) return "mkdir_target";
    if (bound() && unmount(MOUNT_POINT, 0) != 0 && unmount(MOUNT_POINT, MNT_FORCE) != 0) return "unbind";
    iov[0] = (struct iovec){fstype, sizeof fstype};
    iov[1] = (struct iovec){nullfs, sizeof nullfs};
    iov[2] = (struct iovec){from, sizeof from};
    iov[3] = (struct iovec){source, sizeof source};
    iov[4] = (struct iovec){fspath, sizeof fspath};
    iov[5] = (struct iovec){target, sizeof target};
    if (nmount(iov, 6, 0) != 0) return "nmount";
    if (access(MOUNT_POINT "/eboot.bin", F_OK) != 0) {
        unmount(MOUNT_POINT, MNT_FORCE);
        return "bind_hides_eboot";
    }
    return NULL;
}

/* ---- Sony's installer ------------------------------------------------------ */

static void step(const char *what)
{
    crash_stage(what);
    say("tile: %s", what);
    checkpoint();
}

static void *appinst_symbol(const char *name)
{
    return dlsym(RTLD_DEFAULT, name);
}

static int registered(void)
{
    return access(REGISTERED_MARK, F_OK) == 0;
}

static const char *register_title(void)
{
    init_fn initialize = (init_fn)appinst_symbol("sceAppInstUtilInitialize");
    install_dir_fn install_dir = (install_dir_fn)appinst_symbol("sceAppInstUtilAppInstallTitleDir");
    install_all_fn install_all = (install_all_fn)appinst_symbol("sceAppInstUtilAppInstallAll");
    int result;

    say("tile: initialize %s, install-dir %s, install-all %s", initialize ? "found" : "missing", install_dir ? "found" : "missing",
        install_all ? "found" : "missing");
    checkpoint();
    if (!install_dir && !install_all) return "no_install_api";
    if (initialize) {
        step("calling the installer's initialize");
        say("tile: initialize returned %#x", (unsigned)initialize());
    }
    step(install_dir ? "calling install-dir" : "calling install-all");
    result = install_dir ? install_dir(TITLE, "/user/app/", 0) : install_all(NULL);
    usleep(SONY_SETTLE_US);
    say("tile: the installer returned %#x", (unsigned)result);
    checkpoint();
    return result == 0 || (unsigned)result == APPINST_ALREADY ? NULL : "install_refused";
}

static void *install_thread(void *unused)
{
    const char *failure;
    int again = registered();

    (void)unused;
    if (!g_elevated) {
        say("tile: skipped, this process has no rights for it");
        set_state("failed", "no_rights");
        return NULL;
    }
    if (bound() && again) {
        say("tile: already on the home screen");
        set_state("installed", "");
        return NULL;
    }
    say("tile: %s", again ? "binding again after a reboot" : "registering");
    set_state("working", "");
    step("writing its files");
    if (write_files() != 0) {
        say("tile: writing its files failed: errno %d", errno);
        set_state("failed", "files");
        return NULL;
    }
    step("binding the folder");
    failure = bind_folder();
    if (failure) {
        say("tile: bind failed at %s: errno %d", failure, errno);
        set_state("failed", failure);
        return NULL;
    }
    if (!again) {
        step("finding the installer");
        failure = register_title();
    }
    if (failure) {
        unmount(MOUNT_POINT, MNT_FORCE);
        say("tile: %s", failure);
        set_state("failed", failure);
        return NULL;
    }
    if (!again && write_file(REGISTERED_MARK, "1", 1) != 0) say("tile: the mark could not be written: errno %d", errno);
    crash_stage("after the icon");
    say("home-screen icon is in place");
    checkpoint();
    set_state("installed", "");
    return NULL;
}

void tile_install(void)
{
    pthread_attr_t attr;
    pthread_t thread;

    pthread_mutex_lock(&g_lock);
    if (!strcmp(g_state, "working")) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    snprintf(g_state, sizeof g_state, "working");
    pthread_mutex_unlock(&g_lock);
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, TILE_STACK);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, install_thread, NULL) != 0) set_state("failed", "no_thread");
    pthread_attr_destroy(&attr);
}

void tile_resume(void)
{
    if (registered()) tile_install();
    else say("tile: not registered; the page's button adds it");
}

/* Sony's uninstall takes the ShellCore identity, for the one call and no longer: a
 * process that keeps it was seen to black-screen the console. */
static int uninstall(void)
{
    uninstall_fn remove_title = (uninstall_fn)appinst_symbol("sceAppInstUtilAppUnInstall");
    init_fn initialize = (init_fn)appinst_symbol("sceAppInstUtilInitialize");
    pid_t pid = getpid();
    uint64_t saved;
    int result, attempt;

    if (!remove_title) {
        say("tile: no uninstall function");
        return -1;
    }
    if (initialize) initialize();
    step("calling uninstall");
    saved = kernel_get_ucred_authid(pid);
    if (saved) kernel_set_ucred_authid(pid, SHELLCORE_AUTHID);
    result = remove_title(TITLE, NULL, NULL);
    if (saved)
        for (attempt = 0; attempt < 3 && kernel_get_ucred_authid(pid) != saved; attempt++) kernel_set_ucred_authid(pid, saved);
    usleep(SONY_SETTLE_US);
    say("tile: uninstall returned %#x", (unsigned)result);
    return result;
}

int tile_remove(void)
{
    int result;

    say("tile: removing");
    checkpoint();
    set_state("working", "");
    if (bound() && unmount(MOUNT_POINT, 0) != 0 && unmount(MOUNT_POINT, MNT_FORCE) != 0) {
        say("tile: unbind failed: errno %d", errno);
        set_state("failed", "unbind");
        return -1;
    }
    /* Never accepted by Sony: only staged files are left to clear. */
    result = registered() ? uninstall() : 0;
    if (result != 0) {
        /* Its files stay: a working icon beats a grey one. */
        set_state("failed", "uninstall_refused");
        return -1;
    }
    remove_tree(APPMETA, 0);
    remove_tree(USER_APP, 0);
    remove_tree(TILE_DIR, 0);
    unlink(REGISTERED_MARK);
    say("home-screen icon removed");
    checkpoint();
    set_state("removed", "");
    return 0;
}
