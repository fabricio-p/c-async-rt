#include <arpa/inet.h>
#include <inttypes.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <threads.h>
#include <stdatomic.h>
#include <stdio.h>

#include "liburing.h"

#include "async.h"
#include "err_utils.h"

// TODO: Function for directly spawning detached tasks in context

#define LOG_INFO_(fmtstr)                                                   \
    fprintf(stderr, "[INFO:" __FILE__ ":%d] " fmtstr, __LINE__)

#define LOG_INFO(fmtstr, ...)                                               \
    fprintf(stderr, "[INFO:" __FILE__ ":%d] " fmtstr, __LINE__, __VA_ARGS__)

struct io_uring_user_data {
    // int _kind;
    atomic_int is_done;
    int res;
    int flags;
};

void
spawn_discardable(
    ARTCoro *coro,
    ARTCoroFunctionPtr f,
    OpaqueMemory a
) {
    art_coro_init(coro, f, a, (1 << ART_CO_DISCARD) | (1 << ART_CO_FREE));
}

typedef struct {
    union {
        struct in_addr v4;
        struct in6_addr v6;
    } ip;
    int is_ipv4;
} IpAddr;

typedef struct {
    struct sockaddr addr;
    socklen_t addrlen;
    ARTCoroFunctionPtr handler;

    int socket_fd;
    struct io_uring_user_data accept_res;
    int accept_timeout;

    struct io_uring uring;

    int addrinfo_status;
} Server;

typedef struct {
    Server *server;
    // struct sockaddr_in addr;
    int fd;
} HandlerArgument;

typedef enum server_status_t {
    SERVER_OK = 0,
    SERVER_GETADDRINFO_FAIL,
    SERVER_SOCKOPT_FAIL,
    SERVER_BIND_FAIL,
    SERVER_LISTEN_FAIL,
    SERVER_URING_FAIL
} ServerStatus;

typedef struct {
    uint16_t port;
    int backlog;
    int accept_timeout;
    int depth;
} ServerSettings;

void
server_settings_default_init(ServerSettings *settings) {
    settings->port = 0;
    settings->backlog = 10;
    settings->accept_timeout = 50;
    settings->depth = 5;
}

ServerStatus
server_init(
    Server *server,
    ARTCoroFunctionPtr handler,
    ServerSettings const *settings
) {
    ServerStatus status = SERVER_OK;

    char port_str[(int)ceil(log10(UINT16_MAX)) + 1];
    snprintf(port_str, sizeof(port_str), "%" PRIu16, settings->port);

    struct addrinfo hints, *servinfo;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    // TODO: Handle error
    server->addrinfo_status = getaddrinfo(NULL, port_str, &hints, &servinfo);
    THROW_IF(server->addrinfo_status != 0, SERVER_GETADDRINFO_FAIL, 0);

    struct addrinfo *ainfo;
    int socket_fd = -1;
    for (ainfo = servinfo; ainfo != NULL; ainfo = ainfo->ai_next) {
        socket_fd =
            socket(ainfo->ai_family, ainfo->ai_socktype, ainfo->ai_protocol);
        if (socket_fd == -1) {
            continue;
        }

        int is_addr_reused = 1;
        THROW_IF(
            setsockopt(
                socket_fd,
                SOL_SOCKET,
                SO_REUSEADDR,
                &is_addr_reused,
                sizeof(is_addr_reused)
            ) == -1,
            SERVER_SOCKOPT_FAIL,
            1
        );

        if (bind(socket_fd, ainfo->ai_addr, ainfo->ai_addrlen) == -1) {
            close(socket_fd);
            continue;
        }

        break;
    }

    freeaddrinfo(servinfo);
    servinfo = NULL;

    THROW_IF(ainfo == NULL, SERVER_BIND_FAIL, 0);
    THROW_IF(listen(socket_fd, settings->backlog) == -1, SERVER_LISTEN_FAIL, 2);

    server->addr = *ainfo->ai_addr;
    server->addrlen = ainfo->ai_addrlen;
    server->handler = handler;
    server->socket_fd = socket_fd;
    server->accept_res.is_done = 0;
    server->accept_timeout = settings->accept_timeout;

    THROW_IF(
        io_uring_queue_init(settings->depth, &server->uring, 0) < 0,
        SERVER_URING_FAIL,
        2
    );

    RETURN(0);

catch_2:
    if (socket_fd != -1) { close(socket_fd); }
catch_1:
    if (servinfo != NULL) { freeaddrinfo(servinfo); }
catch_0:
    return status;
}

void
server_cleanup(Server *server) {
    close(server->socket_fd);
    server->socket_fd = -1;
    server->addrinfo_status = 0;
    memset(&server->addr, 0, sizeof(server->addr));
    server->handler = NULL;
}

static int
submit_accept(
    struct io_uring *uring,
    int socket_fd,
    struct sockaddr *addr,
    socklen_t addrlen,
    struct io_uring_user_data *res,
    int n_wait
) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(uring);
    if (sqe == NULL) { return -1; }
    atomic_store(&res->is_done, 0);
    io_uring_prep_accept(sqe, socket_fd, addr, &addrlen, 0);
    io_uring_sqe_set_data(sqe, res);
    io_uring_submit_and_wait(uring, n_wait);

    return 0;
}

ART_DEFINE_CO(server_run) {
    Server *server =
        _coro_state_->data == NULL ? NULL : *(Server **)_coro_state_->data;
    ART_CO_INIT_BEGIN(void, Server *, Server *); {
        LOG_INFO("coro_argument = %p\n", (void *)coro_argument);
        LOG_INFO("*coro_argument = %p\n", (void *)*coro_argument);
        *coro_data = *coro_argument;
    } ART_CO_INIT_END();

    ART_CO_STAGE(1) {
        LOG_INFO_("Submitting accept op\n");
        submit_accept(
            &server->uring,
            server->socket_fd,
            &server->addr,
            server->addrlen,
            &server->accept_res,
            1
        );
        LOG_INFO_("Submitted accept op\n");
        ART_CO_YIELD(2);
    }

    ART_CO_STAGE(2) {
        struct io_uring_cqe *cqe;
        while (io_uring_peek_cqe(&server->uring, &cqe) == 0) {
            struct io_uring_user_data *user_data = io_uring_cqe_get_data(cqe);
            if (user_data != NULL) {
                user_data->flags = cqe->flags;
                user_data->res = cqe->res;
            } else {
                LOG_INFO_("submitter is on DnD\n");
            }
            atomic_store(&user_data->is_done, 1);
            io_uring_cqe_seen(&server->uring, cqe);
        }
        ART_CO_YIELD(3);
    }

    ART_CO_STAGE(3) {
        if (atomic_load(&server->accept_res.is_done) != 0) {
            if (server->accept_res.res < 0) {
                LOG_INFO(
                    "accept failed [%s]\n",
                    strerror(-server->accept_res.res)
                );
                ART_CO_YIELD(4);
            }
            LOG_INFO("connection accepted (%d)\n", server->accept_res.res);
            HandlerArgument arg = {
                .fd = server->accept_res.res,
                .server = server,
            };
            ARTCoro coro;
            spawn_discardable(&coro, server->handler, &arg);
        } else {
            ART_CO_YIELD(2);
        }
    }

    ART_CO_STAGE(4) {
        submit_accept(
            &server->uring,
            server->socket_fd,
            &server->addr,
            server->addrlen,
            &server->accept_res,
            1
        );
        ART_CO_YIELD(2);
    }

    ART_CO_END();
}

typedef struct {
    int fd;
    struct io_uring_user_data res;
    struct io_uring *uring;

    uint8_t buffer[256];
    struct timespec timer;
} HandlerState;

#include <time.h>

ART_DEFINE_CO(handler) {
    enum {
        REQ_RECV_PING = 1, RECV_PING,
        REQ_SEND_PONG, SENT_PONG,
        TIMER_WAIT,
        REQ_CLOSE, CLOSED
    };
    ART_CO_INIT_BEGIN(void, HandlerState, HandlerArgument); {
        coro_data->fd = coro_argument->fd;
        coro_data->uring = &coro_argument->server->uring;
        memset(coro_data->buffer, 0, sizeof(coro_data->buffer));
    } ART_CO_INIT_END();

    ART_CO_STAGE(REQ_RECV_PING) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(coro_data->uring);
        if (sqe == NULL) { ART_CO_YIELD(REQ_RECV_PING); }
        atomic_store(&coro_data->res.is_done, 0);
        LOG_INFO_("Gonna receive brb\n");
        io_uring_prep_recv(sqe, coro_data->fd, coro_data->buffer, 256, 0);
        io_uring_sqe_set_data(sqe, &coro_data->res);
        io_uring_submit(coro_data->uring);
        ART_CO_YIELD(RECV_PING);
    }

    ART_CO_STAGE(RECV_PING) {
        if (atomic_load(&coro_data->res.is_done) == 0) {
            ART_CO_YIELD(RECV_PING);
        }
        if (coro_data->res.res < 0) {
            LOG_INFO("recv failed: %s\n", strerror(-coro_data->res.res));
            ART_CO_YIELD(REQ_CLOSE);
        }
        LOG_INFO("buffer: %s\n", coro_data->buffer);
        if (memcmp(coro_data->buffer, "ping", 4) == 0) {
            LOG_INFO_("Got pinged\n");
            ART_CO_YIELD(REQ_SEND_PONG);
        }
        LOG_INFO_("No pongs :(\n");
        ART_CO_YIELD(REQ_CLOSE);
    }

    ART_CO_STAGE(REQ_SEND_PONG) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(coro_data->uring);
        if (sqe == NULL) { ART_CO_YIELD(REQ_SEND_PONG); }
        atomic_store(&coro_data->res.is_done, 0);
        LOG_INFO_("Gonna send pong\n");
        io_uring_prep_send(sqe, coro_data->fd, "pong", 4, 0);
        io_uring_sqe_set_data(sqe, &coro_data->res);
        io_uring_submit(coro_data->uring);
        ART_CO_YIELD(SENT_PONG);
    }

    ART_CO_STAGE(SENT_PONG) {
        if (atomic_load(&coro_data->res.is_done) == 0) {
            ART_CO_YIELD(SENT_PONG);
        }
        if (coro_data->res.res < 0) {
            LOG_INFO("send failed: %s\n", strerror(-coro_data->res.res));
            ART_CO_YIELD(REQ_CLOSE);
        }
        LOG_INFO("Ponged (%d)\n", coro_data->res.res);
        clock_gettime(CLOCK_MONOTONIC, &coro_data->timer);
        ART_CO_YIELD(TIMER_WAIT);
    }

    ART_CO_STAGE(TIMER_WAIT) {
#define TIME_SECOND 1000000000
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t const start =
            coro_data->timer.tv_sec * TIME_SECOND + coro_data->timer.tv_nsec;
        uint64_t const end = now.tv_sec * TIME_SECOND + now.tv_nsec;
        uint64_t const diff = end - start;

        if (diff < 10ull * TIME_SECOND) {
            ART_CO_YIELD(TIMER_WAIT);
        }
    }

    ART_CO_STAGE(REQ_CLOSE) {
        LOG_INFO_("Closing connection\n");
        struct io_uring_sqe *sqe = io_uring_get_sqe(coro_data->uring);
        if (sqe == NULL) { ART_CO_YIELD(REQ_CLOSE); }
        atomic_store(&coro_data->res.is_done, 0);
        io_uring_prep_close(sqe, coro_data->fd);
        io_uring_sqe_set_data(sqe, &coro_data->res);
        io_uring_submit(coro_data->uring);
        ART_CO_YIELD(CLOSED);
    }

    ART_CO_STAGE(CLOSED) {
        if (atomic_load(&coro_data->res.is_done) == 0) {
            ART_CO_YIELD(CLOSED);
        }
        LOG_INFO("Connection closed (%d)\n", coro_data->res.res);
    }

    ART_CO_END();
}

void server_destroy(OpaqueMemory x) {
    Server *server = x;
    server_cleanup(server); free(server);
}

int main() {
    int status = 0;
    LOG_INFO_("Queue initialized\n");
    ARTContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ARTContextSettings ctx_settings;
    memset(&ctx_settings, 0, sizeof(ctx_settings));
    ctx_settings.sched_count = 4;
    ctx_settings.use_current_thread = true;
    art_context_init(&ctx, &ctx_settings);

    Server *server = malloc(sizeof(Server));
    ServerSettings settings;
    server_settings_default_init(&settings);
    settings.depth = 64;
    settings.port = 0xfab;
    ServerStatus server_status = server_init(server, handler, &settings);
    LOG_INFO("server init: %d\n", server_status);
    THROW_IF(server_status != SERVER_OK, 1, 0);
    LOG_INFO_("Server initialized\n");

    ARTCoro *server_coro = art_context_new_coro(&ctx);
    art_coro_init(server_coro, server_run, &server, 1 << ART_CO_DISCARD);
    LOG_INFO_("ARTCoro initialized\n");
    art_context_run_coro(&ctx, server_coro);
    art_context_register_cleanup(&ctx, server_destroy, server);
    art_context_join(&ctx);

    // art_context_cleanup(&ctx);
catch_0:
    return status;
}
