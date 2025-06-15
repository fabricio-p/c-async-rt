#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "async.h"
#include "coro.h"
#include "err_utils.h"
#include "logging.h"

void *get_in_addr(struct sockaddr *sa) {
    return
        sa->sa_family == AF_INET
        ? (void *) &(((struct sockaddr_in *)sa)->sin_addr)
        : (void *) &(((struct sockaddr_in6 *)sa)->sin6_addr);
}

typedef struct {
    char const *address;
    char const *port;
} RequestInfo;

ART_DEFINE_CO(client) {
    typedef struct {
        int socket;
        RequestInfo req_info;
    } State;
    enum {
        STG_CONNECT = 1,
        STG_RECV,
        STG_CLOSE
    };
    ART_CO_INIT_BEGIN(void, State, RequestInfo); {
        coro_data->req_info = *coro_argument;
    }
    ART_CO_STAGE(STG_CONNECT) {
        struct addrinfo hints;

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        struct addrinfo *server_info;
        int errcode = getaddrinfo(
            coro_data->req_info.address,
            coro_data->req_info.port,
            &hints,
            &server_info
        );
        if (errcode != 0) {
            LOG_INFO("getaddrinfo: %s\n", gai_strerror(errcode));
            ART_CO_RETURN(-1);
        }

        struct addrinfo *p = server_info;
        for (; p != NULL; p = p->ai_next) {
            int fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (fd == -1) {
                LOG_INFO_("failed socket\n");
                continue;
            }

            if (connect(fd, p->ai_addr, p->ai_addrlen) == -1) {
                LOG_INFO_("failed connect\n");
                continue;
            }

            coro_data->socket = fd;
            break;
        }

        if (p == NULL) {
            LOG_INFO_("client failed to connect\n");
        }

        char s[INET6_ADDRSTRLEN];
        inet_ntop(
            p->ai_family,
            get_in_addr((struct sockaddr *)p->ai_addr),
            s,
            sizeof(s)
        );
        LOG_INFO("client connecting to %s\n", s);
        freeaddrinfo(server_info);

        ART_CO_YIELD(STG_RECV);
    }

    ART_CO_POLL_IO(STG_RECV, READ, .once = true, .fd = coro_data->socket) {
#define BUFFER_SIZE 0x10000
        char buffer[BUFFER_SIZE];
        ssize_t n = recv(coro_data->socket, buffer, BUFFER_SIZE - 1, 0);
        buffer[n] = 0;
        LOG_INFO("client received: (%" PRIiMAX "): %.*s\n", n, (int)n, buffer);
        ART_CO_YIELD(STG_CLOSE);
#undef BUFFER_SIZE
    }

    ART_CO_STAGE(STG_CLOSE) {
        close(coro_data->socket);
    }

    ART_CO_END();
}

ART_DEFINE_CO(do_thing) {
    typedef struct {
        size_t id;
        struct timespec timer;
    } State;

    ART_CO_INIT_BEGIN(void, State, size_t); {
        coro_data->id = *coro_argument;
        LOG_INFO("STARTING COROUTINE %" PRIuMAX "\n", coro_data->id);
        ART_CO_YIELD(1);
    }
    ART_CO_STAGE(1) {
        LOG_INFO("RUNNING COROUTINE %" PRIuMAX "\n", coro_data->id);
        clock_gettime(CLOCK_MONOTONIC, &coro_data->timer);
        ART_CO_YIELD(2);
    }
    ART_CO_STAGE(2) {
#define TIME_SECOND 1000000000
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t const start =
            coro_data->timer.tv_sec * TIME_SECOND + coro_data->timer.tv_nsec;
        uint64_t const end = now.tv_sec * TIME_SECOND + now.tv_nsec;
        uint64_t const diff = end - start;

        if (diff < (uint64_t)(lrand48() % 8) * TIME_SECOND) {
            ART_CO_YIELD(2);
        }
        ART_CO_YIELD(3);
    }

    ART_CO_STAGE(3) {
        LOG_INFO("FINISHED COROUTINE %" PRIuMAX "\n", coro_data->id);
        ART_CO_RETURN(4);
    }
    ART_CO_END();
}

int main() {
    int status = 0;
    ARTContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ARTContextSettings ctx_settings;
    memset(&ctx_settings, 0, sizeof(ctx_settings));
    ctx_settings.sched_count = 4;
    ctx_settings.use_current_thread = true;
    art_context_init(&ctx, &ctx_settings);

    srand48(time(NULL));
    ARTCoro *coros[] = {
        art_context_new_coro(&ctx),
        art_context_new_coro(&ctx),
        art_context_new_coro(&ctx),
        art_context_new_coro(&ctx),
    };
#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))
    for (size_t i = 0; i < COUNTOF(coros); i++) {
        art_coro_init(coros[i], do_thing, &i, 1 << ART_CO_FREE);
    }
    LOG_INFO_("Pushing coroutines to global queue\n\n\n");
    sleep(2);
    art_context_run_coros(&ctx, coros, 4);
    art_context_join(&ctx);
    return status;
}
