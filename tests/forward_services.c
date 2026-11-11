/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Services on the host's own loopback for the forwarding tests: an echo, and as much
 * of an FTP server as PASV and EPSV need. Each data connection carries one line,
 * "DATA-PASV" or "DATA-EPSV", and the reply's address is 127.0.0.1, as ftpsrv's is
 * when its client is the forwarder. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void services_start(uint16_t echo, uint16_t ftp);

static int listen_on(uint16_t port)
{
    struct sockaddr_in address;
    int fd = socket(AF_INET, SOCK_STREAM, 0), on = 1;

    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&address, sizeof address) != 0 || listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int port_of(int fd)
{
    struct sockaddr_in address;
    socklen_t length = sizeof address;

    getsockname(fd, (struct sockaddr *)&address, &length);
    return ntohs(address.sin_port);
}

static void say_line(int fd, const char *text)
{
    if (write(fd, text, strlen(text)) < 0) return;
}

static void *echo_one(void *opaque)
{
    int fd = (int)(intptr_t)opaque;
    char buffer[65536];
    ssize_t n;

    while ((n = read(fd, buffer, sizeof buffer)) > 0)
        if (write(fd, buffer, (size_t)n) != n) break;
    close(fd);
    return NULL;
}

static int read_line(int fd, char *line, size_t capacity)
{
    size_t used = 0;

    while (used + 1 < capacity) {
        char c;

        if (read(fd, &c, 1) != 1) return -1;
        if (c == '\n') break;
        if (c != '\r') line[used++] = c;
    }
    line[used] = 0;
    return 0;
}

static void *ftp_one(void *opaque)
{
    int fd = (int)(intptr_t)opaque;
    char line[256], reply[128];

    say_line(fd, "220 fake ftp\r\n");
    while (read_line(fd, line, sizeof line) == 0) {
        int extended = !strcmp(line, "EPSV");

        if (extended || !strcmp(line, "PASV")) {
            int passive = listen_on(0), port = port_of(passive), data;
            struct pollfd wait = {passive, POLLIN, 0};

            if (extended) snprintf(reply, sizeof reply, "229 Entering Extended Passive Mode (|||%d|)\r\n", port);
            else snprintf(reply, sizeof reply, "227 Entering Passive Mode (127,0,0,1,%d,%d).\r\n", port >> 8, port & 255);
            say_line(fd, reply);
            if (poll(&wait, 1, 10000) == 1 && (data = accept(passive, NULL, NULL)) >= 0) {
                say_line(data, extended ? "DATA-EPSV" : "DATA-PASV");
                close(data);
                say_line(fd, "226 done\r\n");
            } else {
                say_line(fd, "425 no data connection\r\n");
            }
            close(passive);
        } else if (!strcmp(line, "QUIT")) {
            say_line(fd, "221 bye\r\n");
            break;
        } else {
            say_line(fd, "500 what\r\n");
        }
    }
    close(fd);
    return NULL;
}

struct service {
    int listener;
    void *(*one)(void *);
};

static void *serve(void *opaque)
{
    struct service s = *(struct service *)opaque;

    free(opaque);
    for (;;) {
        pthread_t thread;
        int fd = accept(s.listener, NULL, NULL);

        if (fd < 0) continue;
        if (pthread_create(&thread, NULL, s.one, (void *)(intptr_t)fd) == 0) pthread_detach(thread);
        else close(fd);
    }
    return NULL;
}

static void start(uint16_t port, void *(*one)(void *))
{
    struct service *s = malloc(sizeof *s);
    pthread_t thread;

    s->listener = listen_on(port);
    s->one = one;
    if (s->listener < 0) {
        fprintf(stderr, "services: port %u is held\n", port);
        exit(2);
    }
    pthread_create(&thread, NULL, serve, s);
    pthread_detach(thread);
}

void services_start(uint16_t echo, uint16_t ftp)
{
    start(echo, echo_one);
    start(ftp, ftp_one);
}
