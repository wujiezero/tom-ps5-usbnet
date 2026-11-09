/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See smb_client.h: libsmb2's synchronous calls, one context for each session, used
 * by one thread at a time. libsmb2 keeps one list of every live context, so contexts
 * are made and destroyed under g_lock; so is each connect, since the stack it uses
 * is a setting (smb_net.h) read when the socket is made. */
#include "smb_client.h"
#include "smb_net.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/libsmb2-share-enum.h>
#include <smb2/smb2-errors.h>

#define CONNECT_SECONDS 6               /* a server that is not on that network */
#define REQUEST_SECONDS 30
#define MAX_READ (4u * 1024 * 1024)

/* libsmb2 keeps the last NT status until another replaces it; cleared before each call
 * so it only ever describes that call. Declared in libsmb2-private.h. */
void smb2_set_nterror(struct smb2_context *smb2, int nterror, const char *error_string, ...);
#define FRESH(context) smb2_set_nterror((context), 0, "%s", "")

struct smb_session {
    struct smb2_context *context;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_adapter;

void smb_client_use_adapter(int up)
{
    __atomic_store_n(&g_adapter, up, __ATOMIC_RELAXED);
}

const char *smb_error_code(int error)
{
    switch (error) {
    case SMB_UNREACHABLE: return "unreachable";
    case SMB_AUTH: return "auth";
    case SMB_NO_SHARE: return "no_share";
    case SMB_DENIED: return "nas_denied";
    case SMB_NOT_FOUND: return "not_found";
    case SMB_TIMEOUT: return "timeout";
    case SMB_DROPPED: return "dropped";
    case SMB_NO_MEMORY: return "no_memory";
    default: return "failed";
    }
}

static int from_status(uint32_t status)
{
    switch (status) {
    case SMB2_STATUS_LOGON_FAILURE:
    case SMB2_STATUS_WRONG_PASSWORD:
    case SMB2_STATUS_ACCOUNT_RESTRICTION:
    case SMB2_STATUS_ACCOUNT_DISABLED:
    case SMB2_STATUS_ACCOUNT_EXPIRED:
    case SMB2_STATUS_ACCOUNT_LOCKED_OUT:
    case SMB2_STATUS_PASSWORD_EXPIRED:
    case SMB2_STATUS_PASSWORD_MUST_CHANGE: return SMB_AUTH;
    case SMB2_STATUS_BAD_NETWORK_NAME: return SMB_NO_SHARE;
    case SMB2_STATUS_ACCESS_DENIED:
    case SMB2_STATUS_NETWORK_ACCESS_DENIED:
    case SMB2_STATUS_SHARING_VIOLATION: return SMB_DENIED;
    case SMB2_STATUS_OBJECT_NAME_NOT_FOUND:
    case SMB2_STATUS_OBJECT_PATH_NOT_FOUND:
    case SMB2_STATUS_NO_SUCH_FILE: return SMB_NOT_FOUND;
    case SMB2_STATUS_IO_TIMEOUT: return SMB_TIMEOUT;
    case SMB2_STATUS_NETWORK_NAME_DELETED:
    case SMB2_STATUS_USER_SESSION_DELETED:
    case SMB2_STATUS_CONNECTION_DISCONNECTED:
    case SMB2_STATUS_CONNECTION_RESET: return SMB_DROPPED;
    default: return 0;
    }
}

static int failure(struct smb2_context *context, int rc)
{
    int code = from_status((uint32_t)smb2_get_nterror(context));

    if (code) return code;
    if (smb2_get_fd(context) < 0 || rc == -1) return SMB_DROPPED;
    if (rc == -ENOENT) return SMB_NOT_FOUND;
    if (rc == -EACCES || rc == -EPERM) return SMB_DENIED;
    if (rc == -ETIMEDOUT) return SMB_TIMEOUT;
    if (rc == -ENOMEM) return SMB_NO_MEMORY;
    return SMB_FAILED;
}

static void destroy(struct smb2_context *context)
{
    if (!context) return;
    pthread_mutex_lock(&g_lock);
    smb2_destroy_context(context);
    pthread_mutex_unlock(&g_lock);
}

/* One attempt on one network. anonymous: a NULL password, for servers that take no
 * guest account. */
static struct smb2_context *attempt(const struct smb_server *server, const char *share, int lwip, int anonymous, int *error)
{
    char address[SMB_HOST_BYTES + 8];
    const char *user = server->user[0] ? server->user : "Guest";
    struct smb2_context *context;
    int rc;

    if (server->port && server->port != 445) snprintf(address, sizeof address, "%s:%u", server->host, server->port);
    else snprintf(address, sizeof address, "%s", server->host);
    pthread_mutex_lock(&g_lock);
    context = smb2_init_context();
    if (!context) {
        pthread_mutex_unlock(&g_lock);
        *error = SMB_NO_MEMORY;
        return NULL;
    }
    smb2_set_timeout(context, CONNECT_SECONDS);
    smb2_set_version(context, SMB2_VERSION_ANY);
    smb2_set_security_mode(context, SMB2_NEGOTIATE_SIGNING_ENABLED);
    smb2_set_user(context, user);
    smb2_set_password(context, anonymous ? NULL : server->password);
    if (server->domain[0]) smb2_set_domain(context, server->domain);
    smb_net_use_lwip(lwip);
    rc = smb2_connect_share(context, address, share, user);
    smb_net_use_lwip(0);
    if (rc < 0) {
        int code = from_status((uint32_t)smb2_get_nterror(context));

        *error = code ? code : rc == -ETIMEDOUT ? SMB_TIMEOUT : SMB_UNREACHABLE;
        smb2_destroy_context(context);
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    pthread_mutex_unlock(&g_lock);
    smb2_set_timeout(context, REQUEST_SECONDS);
    *error = 0;
    return context;
}

static struct smb2_context *sign_in(const struct smb_server *server, const char *share, const char **over, int *error)
{
    int tries = __atomic_load_n(&g_adapter, __ATOMIC_RELAXED) ? 2 : 1, i;

    for (i = 0; i < tries; i++) {
        int lwip = tries == 2 && i == 0;
        struct smb2_context *context = attempt(server, share, lwip, 0, error);

        if (!context && !server->password[0] && (*error == SMB_AUTH || *error == SMB_DENIED))
            context = attempt(server, share, lwip, 1, error);
        if (context) {
            if (over) *over = lwip ? "usb" : "console";
            return context;
        }
        /* The server answered: the other network would give the same answer. */
        if (*error != SMB_UNREACHABLE && *error != SMB_TIMEOUT) return NULL;
    }
    return NULL;
}

int smb_connect_share(const struct smb_server *server, const char *share, struct smb_session **out, const char **over)
{
    struct smb_session *s = calloc(1, sizeof *s);
    int error;

    if (!s) return SMB_NO_MEMORY;
    s->context = sign_in(server, share, over, &error);
    if (!s->context) {
        free(s);
        return error;
    }
    *out = s;
    return 0;
}

void smb_disconnect(struct smb_session *s)
{
    if (!s) return;
    (void)smb2_disconnect_share(s->context);
    destroy(s->context);
    free(s);
}

int smb_list_shares(const struct smb_server *server, int (*each)(void *context, const char *name, const char *remark), void *context)
{
    struct smb2_share_enum_reply *reply;
    struct smb2_context *c;
    uint32_t i;
    int error;

    c = sign_in(server, "IPC$", NULL, &error);
    if (!c) return error;
    FRESH(c);
    reply = smb2_share_enum_sync(c, SMB2_SHARE_INFO_1);
    if (!reply) {
        error = failure(c, -EIO);
        destroy(c);
        return error;
    }
    for (i = 0; i < reply->entries_read; i++) {
        struct smb2_share_info_1 *share = &reply->share_info.info_1[i];
        size_t length = share->netname ? strlen(share->netname) : 0;

        if (!length || length >= SMB_NAME_BYTES || share->netname[length - 1] == '$') continue;
        if ((share->type & 3) != SMB2_SHARE_TYPE_DISKTREE || (share->type & SMB2_SHARE_TYPE_HIDDEN)) continue;
        if (each(context, share->netname, share->remark ? share->remark : "")) break;
    }
    smb2_free_data(c, reply);
    (void)smb2_disconnect_share(c);
    destroy(c);
    return 0;
}

static void fill(struct smb_entry *entry, const struct smb2_stat_64 *st)
{
    entry->folder = st->smb2_type == SMB2_TYPE_DIRECTORY;
    entry->size = entry->folder ? 0 : st->smb2_size;
    entry->mtime = (int64_t)st->smb2_mtime;
}

int smb_stat(struct smb_session *s, const char *path, struct smb_entry *entry)
{
    struct smb2_stat_64 st;
    const char *name = strrchr(path, '/');
    int rc;

    memset(&st, 0, sizeof st);
    FRESH(s->context);
    rc = smb2_stat(s->context, path, &st);
    if (rc < 0) return failure(s->context, rc);
    snprintf(entry->name, sizeof entry->name, "%s", name ? name + 1 : path);
    fill(entry, &st);
    return 0;
}

int smb_opendir(struct smb_session *s, const char *path, void **dir)
{
    FRESH(s->context);
    *dir = smb2_opendir(s->context, path);
    return *dir ? 0 : failure(s->context, -EIO);
}

int smb_readdir(struct smb_session *s, void *dir, struct smb_entry *entry)
{
    struct smb2dirent *e;

    while ((e = smb2_readdir(s->context, dir)) != NULL) {
        const unsigned char *p;
        int bad = 0;

        if (!e->name || !strcmp(e->name, ".") || !strcmp(e->name, "..") || strlen(e->name) >= sizeof entry->name) continue;
        /* A server's name is joined into a console path: never one that could climb. */
        for (p = (const unsigned char *)e->name; *p; p++)
            if (*p < 0x20 || *p == 0x7f || *p == '/' || *p == '\\') bad = 1;
        if (bad) continue;
        snprintf(entry->name, sizeof entry->name, "%s", e->name);
        fill(entry, &e->st);
        return 1;
    }
    return 0;
}

void smb_closedir(struct smb_session *s, void *dir)
{
    if (dir) smb2_closedir(s->context, dir);
}

int smb_open_file(struct smb_session *s, const char *path, void **file)
{
    FRESH(s->context);
    *file = smb2_open(s->context, path, O_RDONLY);
    return *file ? 0 : failure(s->context, -EIO);
}

long long smb_read(struct smb_session *s, void *file, void *buffer, size_t length, uint64_t offset)
{
    int rc;

    FRESH(s->context);
    rc = smb2_pread(s->context, file, buffer, (uint32_t)length, offset);
    return rc < 0 ? failure(s->context, rc) : rc;
}

void smb_close_file(struct smb_session *s, void *file)
{
    if (file) (void)smb2_close(s->context, file);
}

size_t smb_read_size(struct smb_session *s)
{
    uint32_t most = smb2_get_max_read_size(s->context);

    return most && most < MAX_READ ? most : MAX_READ;
}
