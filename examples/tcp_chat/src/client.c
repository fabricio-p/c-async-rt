#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>

#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <poll.h>

#include "err_utils.h"
#include "logging.h"
#include "url.h"

void *get_in_addr(struct sockaddr *sa) {
    return
        sa->sa_family == AF_INET
        ? (void *) &(((struct sockaddr_in *)sa)->sin_addr)
        : (void *) &(((struct sockaddr_in6 *)sa)->sin6_addr);
}

int char_range_len(CharRange cr) { return cr.end - cr.begin; }

int
client_connect_to(ARTUrl const *url) {
    int socket_fd = -1;

    struct addrinfo hints;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *server_info;
    char address[INET6_ADDRSTRLEN+1];
    char port[6];
    memcpy(address, url->host.begin, char_range_len(url->host));
    address[char_range_len(url->host)] = '\0';
    snprintf(port, 5, "%" PRIu16, url->port);
    int errcode = getaddrinfo(
        address,
        port,
        &hints,
        &server_info
    );
    if (errcode != 0) {
        LOG_INFO("getaddrinfo[client]: %s\n", gai_strerror(errcode));
        return -1;
    }

    struct addrinfo *p = server_info;
    for (; p != NULL; p = p->ai_next) {
        int fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == -1) {
            LOG_INFO_("[client]: failed socket\n");
            continue;
        }

        if (connect(fd, p->ai_addr, p->ai_addrlen) == -1) {
            LOG_INFO_("[client]: failed connect\n");
            continue;
        }

        socket_fd = fd;
        break;
    }

    if (p == NULL) {
        LOG_INFO_("[client]: failed to find address to connect to\n");
        return -1;
    }

    char s[INET6_ADDRSTRLEN];
    inet_ntop(
        p->ai_family,
        get_in_addr((struct sockaddr *)p->ai_addr),
        s,
        sizeof(s)
    );
    LOG_INFO("[client]: connecting to %s\n", s);
    freeaddrinfo(server_info);

    return socket_fd;
}

CharRange char_range_lit(char const *s) {
    CharRange cr;
    cr.begin = s;
    cr.end = s + strlen(s);
    return cr;
}
#define CHAR_RANGE_FMT(cr) char_range_len(cr), cr.begin

#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))
int main() {
    ARTUrl url = art_url_parse(char_range_lit("http://127.0.0.1:4545/"));
    int
        stdin_fd = fileno(stdin)
    ,   stderr_fd = fileno(stderr)
    ,   stdout_fd = fileno(stdout)
    ,   fd = client_connect_to(&url)
    ;

    LOG_INFO(
        "(ARTUrl) { .scheme = %.*s, .host = %.*s, .path = %.*s, .port = %"
        PRIu16 " }\n",
        CHAR_RANGE_FMT(url.scheme),
        CHAR_RANGE_FMT(url.host),
        CHAR_RANGE_FMT(url.path),
        url.port
    );
    if (fd == -1) {
        LOG_INFO_("[client]: failed to connect\n");
        return -1;
    }
    LOG_INFO("fd = %d\n", fd);

    struct pollfd poll_fds[2] = {0};
    poll_fds[0].fd = stdin_fd;
    poll_fds[0].events = POLLIN;
    poll_fds[1].fd = fd;
    poll_fds[1].events = POLLIN;

#define BUFFER_SIZE 0x1000
    char buf[BUFFER_SIZE];
    for (;;) {
        write(stderr_fd, "> ", 2);
        int ready = poll(poll_fds, 2, -1);
        if (ready < 0) {
            LOG_ERROR(
                "poll[client]: failed with (%d) \"%s\"\n",
                ready,
                strerror(errno)
            );
            close(fd);
            return -1;
        }

        if (ready == 0) {
            LOG_INFO_("poll[client]: timeout\n");
            continue;
        }

        if (poll_fds[0].revents & POLLIN) {
            int n = read(stdin_fd, buf, BUFFER_SIZE);
            if (n < 0) { LOG_INFO_("read[client]: stdin failed\n"); break; }
            if (n == 0) { LOG_INFO_("read[client]: stdin EOF\n"); continue; }
            if (n == 1) {
                LOG_INFO_("NIGGA\n");
                /// write(stderr_fd, "\n", 1);
                continue;
            }
            send(fd, buf, n - 1, 0);
        }

        if (poll_fds[1].revents & POLLIN) {
            int n = recv(fd, buf, BUFFER_SIZE, 0);
            if (n <= 0) { break; }
            write(stderr_fd, "\r<< ", 4);
            write(stdout_fd, buf, n);
            write(stderr_fd, "\n", 1);
        }

        if (poll_fds[1].revents & (POLLHUP | POLLERR)) {
            LOG_INFO_("poll[client]: connection closed\n");
            break;
        }
    }

    return 0;
}
