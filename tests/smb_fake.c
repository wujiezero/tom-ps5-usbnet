/* SPDX-License-Identifier: GPL-3.0-or-later */
/* smb_client.h over a local folder: each folder in smb_fake_root is a share. The
 * server "down.test" never answers, a user "bad" is refused, a file named
 * "broken.bin" fails its second read, and one named "slow.bin" takes 20 ms a read. */
#include "smb_client.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char *smb_fake_root;
size_t smb_fake_read_size = 64 * 1024;
char smb_fake_password[SMB_PASSWORD_BYTES];
int smb_fake_sessions;                  /* open now */

struct smb_session {
    char base[1024];
};

struct fake_dir {
    DIR *d;
    char path[1024];
};

struct fake_file {
    int fd, broken, slow, reads;
};

const char *smb_error_code(int error)
{
    return error == SMB_UNREACHABLE ? "unreachable" : error == SMB_AUTH ? "auth" : error == SMB_NO_SHARE ? "no_share" : error == SMB_NOT_FOUND ? "not_found" : error == SMB_DROPPED ? "dropped" : "failed";
}

void smb_client_use_adapter(int up)
{
    (void)up;
}

static int sign_in(const struct smb_server *server)
{
    if (!strcmp(server->host, "down.test")) return SMB_UNREACHABLE;
    if (!strcmp(server->user, "bad")) return SMB_AUTH;
    snprintf(smb_fake_password, sizeof smb_fake_password, "%s", server->password);
    return 0;
}

int smb_connect_share(const struct smb_server *server, const char *share, struct smb_session **out, const char **over)
{
    struct smb_session *s;
    struct stat st;
    int error = sign_in(server);

    if (error) return error;
    s = calloc(1, sizeof *s);
    snprintf(s->base, sizeof s->base, "%s/%s", smb_fake_root, share);
    if (stat(s->base, &st) != 0 || !S_ISDIR(st.st_mode)) {
        free(s);
        return SMB_NO_SHARE;
    }
    if (over) *over = "console";
    smb_fake_sessions++;
    *out = s;
    return 0;
}

void smb_disconnect(struct smb_session *s)
{
    if (!s) return;
    smb_fake_sessions--;
    free(s);
}

int smb_list_shares(const struct smb_server *server, int (*each)(void *context, const char *name, const char *remark), void *context)
{
    DIR *d;
    struct dirent *e;
    int error = sign_in(server);

    if (error) return error;
    d = opendir(smb_fake_root);
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);

        if (e->d_name[0] == '.' || e->d_name[n - 1] == '$') continue;
        if (each(context, e->d_name, "a \"quoted\" remark")) break;
    }
    closedir(d);
    return 0;
}

static void full(struct smb_session *s, const char *path, char *out)
{
    snprintf(out, 2048, "%s%s%s", s->base, path[0] ? "/" : "", path);
}

int smb_stat(struct smb_session *s, const char *path, struct smb_entry *entry)
{
    char where[2048];
    struct stat st;
    const char *name = strrchr(path, '/');

    full(s, path, where);
    if (stat(where, &st) != 0) return SMB_NOT_FOUND;
    snprintf(entry->name, sizeof entry->name, "%s", name ? name + 1 : path);
    entry->folder = S_ISDIR(st.st_mode);
    entry->size = entry->folder ? 0 : (uint64_t)st.st_size;
    entry->mtime = st.st_mtime;
    return 0;
}

int smb_opendir(struct smb_session *s, const char *path, void **dir)
{
    struct fake_dir *d = calloc(1, sizeof *d);
    char where[2048];

    full(s, path, where);
    d->d = opendir(where);
    if (!d->d) {
        free(d);
        return SMB_NOT_FOUND;
    }
    snprintf(d->path, sizeof d->path, "%s", path);
    *dir = d;
    return 0;
}

int smb_readdir(struct smb_session *s, void *dir, struct smb_entry *entry)
{
    struct fake_dir *d = dir;
    struct dirent *e;

    while ((e = readdir(d->d)) != NULL) {
        char inner[1024];

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(inner, sizeof inner, "%s%s%s", d->path, d->path[0] ? "/" : "", e->d_name);
        if (smb_stat(s, inner, entry) != 0) continue;
        return 1;
    }
    return 0;
}

void smb_closedir(struct smb_session *s, void *dir)
{
    struct fake_dir *d = dir;

    (void)s;
    closedir(d->d);
    free(d);
}

int smb_open_file(struct smb_session *s, const char *path, void **file)
{
    struct fake_file *f = calloc(1, sizeof *f);
    char where[2048];
    const char *name = strrchr(path, '/');

    full(s, path, where);
    name = name ? name + 1 : path;
    f->fd = open(where, O_RDONLY);
    if (f->fd < 0) {
        free(f);
        return SMB_NOT_FOUND;
    }
    f->broken = !strcmp(name, "broken.bin");
    f->slow = !strcmp(name, "slow.bin");
    *file = f;
    return 0;
}

long long smb_read(struct smb_session *s, void *file, void *buffer, size_t length, uint64_t offset)
{
    struct fake_file *f = file;

    (void)s;
    if (f->broken && ++f->reads == 2) return SMB_DROPPED;
    if (f->slow) usleep(20000);
    return pread(f->fd, buffer, length, (off_t)offset);
}

void smb_close_file(struct smb_session *s, void *file)
{
    struct fake_file *f = file;

    (void)s;
    close(f->fd);
    free(f);
}

size_t smb_read_size(struct smb_session *s)
{
    (void)s;
    return smb_fake_read_size;
}
