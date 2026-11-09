/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SMB_CLIENT_H
#define SMB_CLIENT_H
/* Reading from a NAS: the few calls smb.c makes, over libsmb2 (src/smb_client.c) or,
 * in the tests, over a local folder. Paths are inside the share, '/' between parts and
 * none at the start ("" is the share itself), already checked by smb.c. */
#include <stddef.h>
#include <stdint.h>

#define SMB_HOST_BYTES 64
#define SMB_USER_BYTES 64
#define SMB_PASSWORD_BYTES 128
#define SMB_NAME_BYTES 256

struct smb_server {
    char host[SMB_HOST_BYTES], user[SMB_USER_BYTES], password[SMB_PASSWORD_BYTES], domain[SMB_USER_BYTES];
    unsigned port;                      /* 0 for 445 */
};

struct smb_entry {
    char name[SMB_NAME_BYTES];
    int folder;
    uint64_t size;
    int64_t mtime;
};

/* Failures, as the page's error codes. */
#define SMB_UNREACHABLE (-1)
#define SMB_AUTH (-2)
#define SMB_NO_SHARE (-3)
#define SMB_DENIED (-4)
#define SMB_NOT_FOUND (-5)
#define SMB_TIMEOUT (-6)
#define SMB_DROPPED (-7)
#define SMB_FAILED (-8)
#define SMB_NO_MEMORY (-9)
const char *smb_error_code(int error);

struct smb_session;
/* Whether the adapter's network is up: then a server is tried there first, and on the
 * console's own network if it does not answer. */
void smb_client_use_adapter(int up);
/* *over is "usb" or "console": the network that reached the server. */
int smb_connect_share(const struct smb_server *server, const char *share, struct smb_session **out, const char **over);
void smb_disconnect(struct smb_session *s);
/* each() for every disk share that is not hidden; a non-zero return stops. */
int smb_list_shares(const struct smb_server *server, int (*each)(void *context, const char *name, const char *remark), void *context);
int smb_stat(struct smb_session *s, const char *path, struct smb_entry *entry);
int smb_opendir(struct smb_session *s, const char *path, void **dir);
/* 1 with an entry, 0 at the end. Names that hold '/', '\' or control bytes are left out. */
int smb_readdir(struct smb_session *s, void *dir, struct smb_entry *entry);
void smb_closedir(struct smb_session *s, void *dir);
int smb_open_file(struct smb_session *s, const char *path, void **file);
/* Bytes read (0 at the end) or a failure. */
long long smb_read(struct smb_session *s, void *file, void *buffer, size_t length, uint64_t offset);
void smb_close_file(struct smb_session *s, void *file);
/* The largest read worth asking for. */
size_t smb_read_size(struct smb_session *s);
#endif
