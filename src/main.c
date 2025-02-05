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
    CoroutineQueue *queue,
    CoroutineFunctionPtr f,
    OpaqueMemory a
) {
    Coroutine *coro = malloc(sizeof(Coroutine));
    coro_init(coro, f, a);
    coro_queue_push_coro(queue, coro, (1 << CORO_DISCARD) | (1 << CORO_FREE));
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
    CoroutineFunctionPtr handler;

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
    CoroutineFunctionPtr handler,
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

DEFINE_CORO(server_run) {
    Server *server =
        _coro_ctx_->state == NULL ? NULL : *(Server **)_coro_ctx_->state;
    CORO_INIT_BEGIN(void, Server *, Server *); {
        LOG_INFO("coro_argument = %p\n", (void *)coro_argument);
        LOG_INFO("*coro_argument = %p\n", (void *)*coro_argument);
        *coro_state = *coro_argument;
    } CORO_INIT_END();

    CORO_STAGE(1) {
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
        CORO_YIELD(2);
    }

    CORO_STAGE(2) {
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
        CORO_YIELD(3);
    }

    CORO_STAGE(3) {
        if (atomic_load(&server->accept_res.is_done) != 0) {
            if (server->accept_res.res < 0) {
                LOG_INFO(
                    "accept failed [%s]\n",
                    strerror(-server->accept_res.res)
                );
                CORO_YIELD(4);
            }
            LOG_INFO("connection accepted (%d)\n", server->accept_res.res);
            HandlerArgument arg = {
                .fd = server->accept_res.res,
                .server = server,
            };
            spawn_discardable(_coro_queue_, server->handler, &arg);
        } else {
            CORO_YIELD(2);
        }
    }

    CORO_STAGE(4) {
        submit_accept(
            &server->uring,
            server->socket_fd,
            &server->addr,
            server->addrlen,
            &server->accept_res,
            1
        );
        CORO_YIELD(2);
    }

    CORO_END();
}

typedef struct {
    int fd;
    struct io_uring_user_data res;
    struct io_uring *uring;

    uint8_t buffer[256];
    struct timespec timer;
} HandlerState;

#include <time.h>

DEFINE_CORO(handler) {
    CORO_INIT_BEGIN(void, HandlerState, HandlerArgument); {
        coro_state->fd = coro_argument->fd;
        coro_state->uring = &coro_argument->server->uring;
        memset(coro_state->buffer, 0, sizeof(coro_state->buffer));
    } CORO_INIT_END();

    CORO_STAGE(1) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(coro_state->uring);
        if (sqe == NULL) { CORO_YIELD(1); }
        atomic_store(&coro_state->res.is_done, 0);
        LOG_INFO_("Gonna receive brb\n");
        io_uring_prep_recv(sqe, coro_state->fd, coro_state->buffer, 256, 0);
        io_uring_sqe_set_data(sqe, &coro_state->res);
        io_uring_submit(coro_state->uring);
        CORO_YIELD(2);
    }

    CORO_STAGE(2) {
        if (atomic_load(&coro_state->res.is_done) == 0) {
            CORO_YIELD(2);
        }
        if (coro_state->res.res < 0) {
            LOG_INFO("recv failed: %s\n", strerror(-coro_state->res.res));
            CORO_YIELD(6);
        }
        LOG_INFO("buffer: %s\n", coro_state->buffer);
        if (memcmp(coro_state->buffer, "ping", 4) == 0) {
            LOG_INFO_("Got pinged\n");
            CORO_YIELD(3);
        }
        LOG_INFO_("No pongs :(\n");
        CORO_YIELD(6);
    }

    CORO_STAGE(3) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(coro_state->uring);
        if (sqe == NULL) { CORO_YIELD(3); }
        atomic_store(&coro_state->res.is_done, 0);
        LOG_INFO_("Gonna send pong\n");
        io_uring_prep_send(sqe, coro_state->fd, "pong", 4, 0);
        io_uring_sqe_set_data(sqe, &coro_state->res);
        io_uring_submit(coro_state->uring);
        CORO_YIELD(4);
    }

    CORO_STAGE(4) {
        if (atomic_load(&coro_state->res.is_done) == 0) {
            CORO_YIELD(4);
        }
        if (coro_state->res.res < 0) {
            LOG_INFO("send failed: %s\n", strerror(-coro_state->res.res));
            CORO_YIELD(6);
        }
        LOG_INFO("Ponged (%d)\n", coro_state->res.res);
        clock_gettime(CLOCK_MONOTONIC, &coro_state->timer);
        CORO_YIELD(5);
    }

    CORO_STAGE(5) {
#define TIME_SECOND 1000000000
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t const start =
            coro_state->timer.tv_sec * TIME_SECOND + coro_state->timer.tv_nsec;
        uint64_t const end = now.tv_sec * TIME_SECOND + now.tv_nsec;
        uint64_t const diff = end - start;

        if (diff < 10ull * TIME_SECOND) {
            CORO_YIELD(5);
        }
    }

    CORO_STAGE(6) {
        LOG_INFO_("Closing connection\n");
        struct io_uring_sqe *sqe = io_uring_get_sqe(coro_state->uring);
        if (sqe == NULL) { CORO_YIELD(6); }
        atomic_store(&coro_state->res.is_done, 0);
        io_uring_prep_close(sqe, coro_state->fd);
        io_uring_sqe_set_data(sqe, &coro_state->res);
        io_uring_submit(coro_state->uring);
        CORO_YIELD(7);
    }

    CORO_STAGE(7) {
        if (atomic_load(&coro_state->res.is_done) == 0) {
            CORO_YIELD(7);
        }
        LOG_INFO("Connection closed (%d)\n", coro_state->res.res);
    }

    CORO_END();
}

int main() {
    int status = 0;
    CoroutineQueue queue;
    coro_queue_init(&queue);
    LOG_INFO_("Queue initialized\n");

    Server *server = malloc(sizeof(Server));
    ServerSettings settings;
    server_settings_default_init(&settings);
    settings.depth = 64;
    settings.port = 0xfab;
    ServerStatus server_status = server_init(server, handler, &settings);
    LOG_INFO("server init: %d\n", server_status);
    THROW_IF(server_status != SERVER_OK, 1, 0);
    LOG_INFO_("Server initialized\n");

    Coroutine server_coro;
    coro_init(&server_coro, server_run, &server);
    LOG_INFO_("Coroutine initialized\n");

    coro_queue_push_coro(&queue, &server_coro, 1 << CORO_DISCARD);
    LOG_INFO_("Coroutine pushed\n");
    while (!coro_queue_is_empty(&queue)) {
        CoroutineQueue_Node *node = coro_queue_pop(&queue);
        if (coro_run(node->coro, &queue) == CORO_DONE) {
            coro_queue_node_delete(node);
        } else {
            coro_queue_push(&queue, node);
        }
    }
    server_cleanup(server); free(server);
    coro_queue_cleanup(&queue);
catch_0:
    return status;
}
