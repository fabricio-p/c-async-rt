#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>

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
    ctx->scheds_items = calloc(ctx->scheds_size, sizeof(ARTCoroDeque));
    for (size_t i = 0; i < ctx->scheds_size; i++) {
        art_scheduler_init(ctx, &ctx->scheds_items[i]);
        if (i != 0 || !settings->use_current_thread) {
            art_scheduler_start(&ctx->scheds_items[i]);
        }
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
    sched->epoll_fd = epoll_create1(0);
    DARRAY_INIT(sched, epoll_evs_, struct epoll_event, 10);
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
        if (coro_res.status == ART_CO_DONE && coro->flags & (1 << ART_CO_FREE)) {
            art_coro_cleanup(coro);
            // TODO: Don't do this directly like this
            free(coro);
        } else if (
            coro_res.status == ART_CO_POLL_IO_READ
            || coro_res.status == ART_CO_POLL_IO_WRITE
        ) {
            struct epoll_event ev;
            ev.events =
                coro_res.status == ART_CO_POLL_IO_READ ? EPOLLIN : EPOLLOUT;
            if (coro_res.d.io.once) {
                ev.events |= EPOLLONESHOT;
            }
            ev.data.ptr = coro;
            epoll_ctl(sched->epoll_fd, EPOLL_CTL_ADD, coro_res.d.io.fd, &ev);
            art_coro_deque_push_back(&sched->io_q, coro);
        }
        if (coro_res.status != ART_CO_DONE) {
            art_coro_deque_push_back(buffer_q, coro);
        }

poll_events:
        if (queues_swapped) {
            int count = epoll_wait(
                sched->epoll_fd,
                sched->epoll_evs_items,
                sched->epoll_evs_size,
                1
            );
            for (int i = 0; i < count; i++) {
                struct epoll_event *ev = &sched->epoll_evs_items[i];
                ARTCoro *coro = ev->data.ptr;
                art_coro_deque_pluck(&sched->io_q, coro);
                art_coro_deque_push_back(buffer_q, coro);
            }
        }

        {
            art_coro_gqueue_lock(&sched->ctx->global_q);
            size_t count = 0;
            for (;;) {
                count = art_coro_gqueue_fetch_batch(
                    &sched->ctx->global_q,
                    buffer_q,
                    1 // PIPE_BATCH_SIZE
                );
                if (coro == NULL && count == 0) {
                    art_coro_gqueue_wait(&sched->ctx->global_q);
                    continue;
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
    if (coro == NULL) {
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
