#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

#include "async.h"
#include "logging.h"

static void *
art_scheduler_loop(ARTScheduler *sched);

static bool
art_scheduler_next_task(
    ARTCoro **coro,
    ARTScheduler *sched,
    ARTCoroDeque **active_q,
    ARTCoroDeque **buffer_q
);

int
art_context_init(ARTContext *ctx, ARTContextSettings const *settings) {
    ctx->main_thread_id = pthread_self();
    ctx->scheds_size = settings->sched_count;
    ctx->scheds_items = calloc(ctx->scheds_size, sizeof(ARTScheduler));
    LOG_INFO(
        "%" PRIuMAX " schedulers initialized (%p)\n",
        ctx->scheds_size,
        (void *) ctx->scheds_items
    );
    for (size_t i = 0; i < ctx->scheds_size; i++) {
        art_scheduler_init(ctx, &ctx->scheds_items[i]);
    }
    if (settings->use_current_thread) {
        ctx->scheds_items[0].thread_id = ctx->main_thread_id;
    }
    art_coro_gqueue_init(&ctx->global_q);
    LOG_INFO_("Global queue initialized\n");

    LOG_INFO(
        "Context initialized (%" PRIuMAX " schedulers)\n",
        ctx->scheds_size
    );

    return 0;
}

void
art_context_run(ARTContext *ctx) {
    size_t i = 0;
    if (pthread_equal(ctx->main_thread_id, ctx->scheds_items[0].thread_id)) {
        i = 1;
    }
    for (; i < ctx->scheds_size; i++) {
        art_scheduler_start(&ctx->scheds_items[i]);
    }
}

ARTCoro *
art_context_new_coro(__attribute__((unused)) ARTContext *ctx) {
    ARTCoro *coro = malloc(sizeof(ARTCoro));
    return coro;
}

void
art_context_run_coros(ARTContext *ctx, ARTCoro **coros, size_t n) {
    art_coro_gqueue_push(&ctx->global_q, coros, n);
}

void
art_context_register_cleanup(
    ARTContext *ctx,
    void (*fn)(OpaqueMemory),
    OpaqueMemory data
) {
    ARTCleanupEntry *entry = DARRAY_PUSH(ctx, cleanups_, ARTCleanupEntry);
    entry->fn = fn;
    entry->data = data;
}

void
art_context_join(ARTContext *ctx) {
    if (pthread_equal(ctx->main_thread_id, ctx->scheds_items[0].thread_id)) {
        art_scheduler_loop(&ctx->scheds_items[0]);
    } else {
        pthread_join(ctx->scheds_items[0].thread_id, NULL);
    }
}

void
art_context_cleanup(ARTContext *ctx) {
    for (size_t i = 0; i < ctx->scheds_size; i++) {
        art_scheduler_cancel(&ctx->scheds_items[i]);
        art_scheduler_cleanup(&ctx->scheds_items[i]);
    }
    free(ctx->scheds_items);

    for (size_t i = 0; i < ctx->cleanups_size; i++) {
        ctx->cleanups_items[i].fn(ctx->cleanups_items[i].data);
    }
    DARRAY_UNINIT(ctx, cleanups_);

    art_coro_gqueue_cleanup(&ctx->global_q);
}

void
art_scheduler_init(ARTContext *ctx, ARTScheduler *sched) {
    sched->ctx = ctx;
    art_coro_deque_init(&sched->wait_q);
    art_coro_deque_init(&sched->io_q);
    art_coro_deque_init(&sched->active_qs[0]);
    art_coro_deque_init(&sched->active_qs[1]);
    DARRAY_INIT(sched, epoll_evs_, struct epoll_event, 16);
    sched->epoll_evs_size = sched->epoll_evs_capacity;
    sched->epoll_fd = epoll_create1(0);

    // struct epoll_event ev;
    // ev.events = EPOLLIN;
    // ev.data.fd = sched->ctx->global_q.eventfd;
    // epoll_ctl(
    //     sched->epoll_fd, EPOLL_CTL_ADD, sched->ctx->global_q.eventfd, &ev
    // );

    // TODO: Maybe use some id thing
    LOG_INFO(
        "Scheduler initialized (id=%" PRIuMAX ")\n",
        sched - ctx->scheds_items
    );
}

void *
art_scheduler_loop(ARTScheduler *sched) {
    LOG_INFO(
        "Scheduler loop started (id=%" PRIuMAX ")\n",
        sched - sched->ctx->scheds_items
    );
#define PIPE_BATCH_SIZE 10
    pthread_cleanup_push((void (*) (void *))art_scheduler_cleanup, sched);

    for (;;) {
        pthread_testcancel();
        ARTCoroDeque *active_q;
        ARTCoroDeque *buffer_q;
        ARTCoro *coro;
        bool global_queue_ready = true;

        bool queues_swapped = art_scheduler_next_task(
            &coro,
            sched,
            &active_q,
            &buffer_q
        );
        if (coro == NULL) {
            goto poll_events;
        }

        ARTCoroResult coro_res = art_coro_run(sched, coro);
        switch (coro_res.status) {
        case ART_CO_INITIALIZED: break;

        case ART_CO_DONE:
        {
            if (coro->flags & (1 << ART_CO_FREE)) {
                LOG_INFO("Coroutine (id=%" PRIuMAX ") done\n", coro->id);
                art_coro_cleanup(coro);
                free(coro);
            }
        } break;

        case ART_CO_POLL_IO_READ:
        case ART_CO_POLL_IO_WRITE:
        {
            struct epoll_event ev;
            ev.events =
                coro_res.status == ART_CO_POLL_IO_READ ? EPOLLIN : EPOLLOUT;
            ev.events |= EPOLLET;
            if (coro_res.d.io.once) {
                ev.events |= EPOLLONESHOT;
            }
            ev.data.ptr = coro;
            int rv = epoll_ctl(
                sched->epoll_fd, EPOLL_CTL_ADD, coro_res.d.io.fd, &ev
            );
            if (rv < 0) {
                LOG_ERROR("epoll_ctl failed(%d): %s\n", errno, strerror(errno));
            }
            art_coro_deque_push_back(&sched->io_q, coro);
        } break;

        case ART_CO_RUNNING:
            art_coro_deque_push_back(buffer_q, coro);
            break;
        }

poll_events:
        if (queues_swapped/* && sched->io_q.first != NULL*/) {
            int count = epoll_wait(
                sched->epoll_fd,
                sched->epoll_evs_items,
                sched->epoll_evs_size,
                1000 * 2
            );
            LOG_INFO("%d events\n", count);
            if (count < 0) {
                LOG_ERROR(
                    "epoll_wait failed(%d): %s\n",
                    errno,
                    strerror(errno)
                );
                break;
            }
            for (int i = 0; i < count; i++) {
                struct epoll_event *ev = &sched->epoll_evs_items[i];
                // if (ev->data.fd == sched->ctx->global_q.eventfd) {
                //     global_queue_ready = true;
                //     continue;
                // }
                ARTCoro *coro = ev->data.ptr;
                LOG_INFO(
                    "[id=%" PRIuMAX "] Received io event for coroutine (id=%"
                    PRIuMAX ")\n",
                    sched - sched->ctx->scheds_items,
                    coro->id
                );
                art_coro_deque_pluck(&sched->io_q, coro);
                art_coro_deque_push_back(buffer_q, coro);
            }
        }

        if (global_queue_ready) {
            // char _b[sizeof(uint64_t)];
            // read(sched->ctx->global_q.eventfd, _b, sizeof(_b));
            art_coro_gqueue_lock(&sched->ctx->global_q);
            size_t count = 0;
            for (;;) {
                count = art_coro_gqueue_fetch_batch(
                    &sched->ctx->global_q,
                    buffer_q,
                    PIPE_BATCH_SIZE
                );
                if (
                    coro == NULL
                    && count == 0
                    && sched->io_q.first == NULL
                    && sched->wait_q.first == NULL
                ) {
                    art_coro_gqueue_wait(&sched->ctx->global_q);
                    continue;
                }
                if (count != 0) {
                    LOG_INFO(
                        "[id=%" PRIuMAX "] Global queue fetched %"
                        PRIuMAX " coroutines\n",
                        sched - sched->ctx->scheds_items,
                        count
                    );
                }
                break;
            }
            art_coro_gqueue_unlock(&sched->ctx->global_q);
        }
    }
    pthread_cleanup_pop(1);
    return NULL;
}

void
art_scheduler_start(ARTScheduler *sched) {
    LOG_INFO(
        "Starting scheduler (id=%" PRIuMAX ")\n",
        sched - sched->ctx->scheds_items
    );
    pthread_create(
        &sched->thread_id,
        NULL,
        (void *(*) (void *))art_scheduler_loop,
        sched
    );
}

void
art_scheduler_cancel(ARTScheduler *sched) {
    LOG_INFO(
        "Canceling scheduler (id=%" PRIuMAX ")\n",
        sched - sched->ctx->scheds_items
    );
    pthread_cancel(sched->thread_id);
}


void
art_scheduler_cleanup(ARTScheduler *sched) {
    DARRAY_UNINIT(sched, epoll_evs_);
    close(sched->epoll_fd);
    sched->epoll_fd = -1;

    for (
        ARTCoro *coro = sched->io_q.first;
        coro != NULL;
        coro = DOUBLE_LIST_NEXT(coro,)
    ) {
        art_coro_cleanup(coro);
    }
    art_coro_deque_cleanup(&sched->io_q);

    for (
        ARTCoro *coro = sched->wait_q.first;
        coro != NULL;
        coro = DOUBLE_LIST_NEXT(coro,)
    ) {
        art_coro_cleanup(coro);
    }
    art_coro_deque_cleanup(&sched->wait_q);

    for (
        ARTCoro *coro = sched->active_qs[0].first;
        coro != NULL;
        coro = DOUBLE_LIST_NEXT(coro,)
    ) {
        art_coro_cleanup(coro);
    }
    art_coro_deque_cleanup(&sched->active_qs[0]);

    for (
        ARTCoro *coro = sched->active_qs[1].first;
        coro != NULL;
        coro = DOUBLE_LIST_NEXT(coro,)
    ) {
        art_coro_cleanup(coro);
    }
    art_coro_deque_cleanup(&sched->active_qs[1]);
}

static bool
art_scheduler_next_task(
    ARTCoro **coro,
    ARTScheduler *sched,
    ARTCoroDeque **active_q,
    ARTCoroDeque **buffer_q
) {
    size_t active_idx = atomic_load_explicit(
        &sched->active_q_idx,
        memory_order_acquire
    );
    size_t buffer_idx = !active_idx;
    *active_q = &sched->active_qs[active_idx];
    *buffer_q = &sched->active_qs[buffer_idx];
    *coro = art_coro_deque_pop_front_lock(*active_q);
    if (*coro == NULL) {
        active_idx = !active_idx;
        buffer_idx = !buffer_idx;
        atomic_store_explicit(
            &sched->active_q_idx,
            active_idx,
            memory_order_release
        );
        *active_q = &sched->active_qs[active_idx];
        *buffer_q = &sched->active_qs[buffer_idx];
        *coro = art_coro_deque_pop_front_lock(*active_q);
        return true;
    }
    return false;
}
