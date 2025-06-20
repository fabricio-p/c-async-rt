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

#include "async.h"
#include "coro.h"
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

ART_DEFINE_CO(do_handler_stuff) {
    typedef struct {
        int fd;
    } State;
    enum { STG_RECV = 1, STG_SEND, STG_CLOSE };

    ART_CO_INIT_BEGIN(void, State, int); {
        coro_data->fd = *coro_argument;
    } ART_CO_INIT_END();

    ART_CO_POLL_IO_STAGE(
        STG_RECV, .once = true, .fd = coro_data->fd, .kind = ART_IO_READ
    ) {
#define BUFFER_SIZE 0x100
        char msg[BUFFER_SIZE];
        ssize_t n = recv(coro_data->fd, msg, BUFFER_SIZE, 0);
        if (n < 0) {
            LOG_ERROR(
                "recv[handler]: failed with (%" PRIiMAX ") \"%s\"\n",
                n,
                strerror(errno)
            );
            ART_CO_YIELD(STG_CLOSE);
        }

        LOG_INFO("recv[handler]: got message \"%.*s\"\n", (int)n, msg);
        if (strncmp(msg, "ping", n) != 0) {
            ART_CO_YIELD(STG_CLOSE);
        }
        LOG_INFO_("recv[handler]: pongin'\n");
#undef BUFFER_SIZE
    }

    ART_CO_POLL_IO_STAGE(
        STG_SEND, .once = true, .fd = coro_data->fd, .kind = ART_IO_WRITE
    ) {
        char const msg[] = "pong";
        ssize_t n = send(coro_data->fd, msg, sizeof(msg), 0);
        if (n < 0) {
            LOG_ERROR(
                "send[handler]: failed with (%" PRIiMAX ") \"%s\"\n",
                n,
                strerror(errno)
            );
        }
        LOG_INFO_("send[handler]: pong\n");
        ART_CO_YIELD(STG_CLOSE);
    }

    ART_CO_STAGE(STG_CLOSE) {
        close(coro_data->fd);
        ART_CO_RETURN(-1);
    }

    ART_CO_END();
}

int server_listen_to(char const *port) {
#define BACKLOG 10
    int socket_fd = -1;
    struct addrinfo hints;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo *server_info;
    int errcode = getaddrinfo(NULL, port, &hints, &server_info);
    if (errcode != 0) {
        LOG_INFO("getaddrinfo[server]: %s\n", gai_strerror(errcode));
        return -1;
    }

    struct addrinfo *p = server_info;
    for (; p != NULL; p = p->ai_next) {
        int fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == -1) {
            LOG_INFO_("[server]: failed socket\n");
            continue;
        }

        int yes = 1;
        if (
            setsockopt(
                fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)
            ) == -1
        ) {
            LOG_INFO_("[server]: failed setsockopt\n");
            continue;
        }

        if (bind(fd, p->ai_addr, p->ai_addrlen) == -1) {
            close(fd);
            LOG_INFO_("[server]: failed bind\n");
            continue;
        }

        socket_fd = fd;
        break;
    }

    freeaddrinfo(server_info);

    if (p == NULL) {
        LOG_INFO_("[server]: failed to find address to bind to\n");
        return -1;
    }

    if (listen(socket_fd, BACKLOG) == -1) {
        LOG_INFO_("[server]: failed listen\n");
        return -1;
    }

    return socket_fd;
}

typedef struct {
    char const *port;
    int socket;
} State;

void
server_cleanup(void *arg) {
    State *state = arg;
    close(state->socket);
}

ART_DEFINE_CO(do_server_stuff) {
    enum { STG_START = 1, STG_ACCEPT };

    ART_CO_INIT_BEGIN(void, State, char const); {
        coro_data->port = coro_argument;
    } ART_CO_INIT_END();

    ART_CO_STAGE(STG_START) {
        coro_data->socket = server_listen_to(coro_data->port);
        if (coro_data->socket == -1) {
            ART_CO_RETURN(-1);
        }

        LOG_INFO_("[server]: waiting for connections...\n");

        art_context_register_cleanup(
            _coro_scheduler_->ctx,
            server_cleanup,
            coro_data
        );

        ART_CO_YIELD(STG_ACCEPT);
    }

    ART_CO_POLL_IO_LOOP_BEGIN(
        STG_ACCEPT, .once = false, .fd = coro_data->socket, .kind = ART_IO_READ
    ) {
        struct sockaddr_storage their_addr;
        socklen_t sin_size = sizeof(their_addr);
        int conn_fd = accept(
            coro_data->socket, (struct sockaddr *)&their_addr, &sin_size
        );

        if (conn_fd == -1) {
            LOG_INFO_("[server]: failed accept\n");
        }

        char s[INET6_ADDRSTRLEN];
        inet_ntop(
            their_addr.ss_family,
            get_in_addr((struct sockaddr *)&their_addr),
            s,
            sizeof(s)
        );
        LOG_INFO("accept[server]: %s\n", s);

        ARTCoro *handler = art_context_new_coro(_coro_scheduler_->ctx);
        art_coro_init(
            handler,
            do_handler_stuff,
            &conn_fd,
            (1 << ART_CO_DISCARD) | (1 << ART_CO_FREE)
        );
        art_context_run_coros(_coro_scheduler_->ctx, &handler, 1);
    } ART_CO_POLL_IO_LOOP_END(STG_ACCEPT);

    ART_CO_END();
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
    LOG_INFO(
        "(ARTUrl) { .scheme = %.*s, .host = %.*s, .path = %.*s, .port = %"
        PRIu16 " }\n",
        CHAR_RANGE_FMT(url.scheme),
        CHAR_RANGE_FMT(url.host),
        CHAR_RANGE_FMT(url.path),
        url.port
    );
    int status = 0;
    ARTContext ctx;
    ARTContextSettings ctx_settings;
    memset(&ctx, 0, sizeof(ctx));
    memset(&ctx_settings, 0, sizeof(ctx_settings));
    ctx_settings.sched_count = 4;
    ctx_settings.use_current_thread = true;
    art_context_init(&ctx, &ctx_settings);

    ARTCoro *server_coro = art_context_new_coro(&ctx);
    art_coro_init(server_coro, do_server_stuff, "4545", 1 << ART_CO_FREE);

    sleep(1);
    LOG_INFO_("Pushing server coroutine to global queue\n");
    art_context_run_coros(&ctx, &server_coro, 1);
    sleep(1);
    art_context_run(&ctx);
    LOG_INFO_("Running context\n\n\n");
    art_context_join(&ctx);
    return status;
}
