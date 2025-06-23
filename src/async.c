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

    pthread_spin_init(
        &ctx->cleanups.lock,
        PTHREAD_PROCESS_PRIVATE
    );
    ctx->n_active_sched = ctx->scheds_size;

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
    pthread_spin_lock(&ctx->cleanups.lock);
    ARTCleanupEntry *entry = DARRAY_PUSH(&ctx->cleanups,, ARTCleanupEntry);
    entry->fn = fn;
    entry->data = data;
    pthread_spin_unlock(&ctx->cleanups.lock);
}

void
art_context_join(ARTContext *ctx) {
    if (pthread_equal(ctx->main_thread_id, ctx->scheds_items[0].thread_id)) {
        art_scheduler_loop(&ctx->scheds_items[0]);
    } else {
        for (size_t i = 0; i < ctx->scheds_size; i++) {
            pthread_join(ctx->scheds_items[i].thread_id, NULL);
        }
    }
}

void
art_context_cleanup(ARTContext *ctx) {
    for (size_t i = 0; i < ctx->cleanups.size; i++) {
        ctx->cleanups.items[i].fn(ctx->cleanups.items[i].data);
    }
    DARRAY_UNINIT(&ctx->cleanups,);

    for (size_t i = 0; i < ctx->scheds_size; i++) {
        art_scheduler_cancel(&ctx->scheds_items[i]);
        art_scheduler_cleanup(&ctx->scheds_items[i]);
    }
    free(ctx->scheds_items);

    art_coro_gqueue_cleanup(&ctx->global_q);
}

void
art_scheduler_init(ARTContext *ctx, ARTScheduler *sched) {
    sched->ctx = ctx;
    art_coro_deque_init(&sched->recv_l);
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

    sched->_fd_bitset = 0;
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
        if (atomic_load(&sched->ctx->n_active_sched) == 0) {
            break;
        }
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
        LOG_INFO(
            "active_q->first = %p, buffer_q->first = %p\n",
            (void *)active_q->first,
            (void *)buffer_q->first
        );
        if (coro == NULL) {
            goto poll_events;
        }
        LOG_INFO("coro.id = %" PRIuMAX "\n", coro->id);

        ARTCoroResult coro_res = art_coro_run(sched, coro);
        LOG_INFO(
            "[coro.id=%" PRIuMAX "] status=%d, stage=%" PRIuMAX "\n",
            coro->id,
            coro_res.status,
            coro_res.stage
        );
        if (sched->ctx->global_q.first == NULL) {
            global_queue_ready = false;
        }

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

        case ART_CO_POLL_IO:
        {
            LOG_INFO(
                "[coro.id=%" PRIuMAX "] fd(%d) POLL_IO\n",
                coro->id,
                coro_res.d.io.fd
            );
            if (coro_res.d.io.kind == ART_IO_REGISTERED) {
                LOG_INFO(
                    "[coro.id=%" PRIuMAX "] fd(%d) ART_IO_REGISTERED\n",
                    coro->id,
                    coro_res.d.io.fd
                );
                goto schedule_io;
            }

            struct epoll_event ev;
            /*
             * NOTE: As background information: The way edge-triggering works is that you keep your socket in the set all the time (so there's no race against epoll_add). The simplest example is a socket where you have a lot of data you want to write, but it could get filled up; you write until the socket buffer is full and get EAGAIN (now the level is low), and then you know that you will get a wakeup through epoll when there's room the next time (the level goes from low to high). With level-triggering, you would have to take the socket in and out of the epoll set all the time, so edge-triggering is more efficient. (But you have to be really careful never to forget about the socket in the high-level state, since you won't ever get any wakeups for it then!) A similar pattern exists for reads, where you could need to pause reading due to your own buffers getting full.
             */
            ev.events |= EPOLLET;
            ev.events |= coro_res.d.io.kind & ART_IO_READ ? EPOLLIN : 0;
            ev.events |= coro_res.d.io.kind & ART_IO_WRITE ? EPOLLOUT : 0;
            ev.events |= coro_res.d.io.once ? EPOLLONESHOT : 0;
            ev.data.ptr = coro;

            int rv = epoll_ctl(
                sched->epoll_fd, EPOLL_CTL_ADD, coro_res.d.io.fd, &ev
            );
            if (rv < 0) {
                LOG_ERROR("epoll_ctl failed(%d): %s\n", errno, strerror(errno));
            }
            if (sched->_fd_bitset & (1 << coro_res.d.io.fd)) {
                LOG_ERROR(
                    "[coro.id=%" PRIuMAX "] fd(%d) already registered\n",
                    coro->id,
                    coro_res.d.io.fd
                );
            } else {
                sched->_fd_bitset |= (1 << coro_res.d.io.fd);
            }

schedule_io:
            LOG_INFO("[coro.id=%" PRIuMAX "] waiting for io...\n", coro->id);
            art_coro_deque_push_back(&sched->io_q, coro);
        } break;

        case ART_CO_RUNNING:
            // LOG_INFO("[coro.id=%" PRIuMAX "] RUNNING, stage=%" PRIuMAX "\n", coro->id, coro->stage);
            art_coro_deque_push_back(buffer_q, coro);
            break;

        case ART_CO_CHAN_SEND:
            break;

        case ART_CO_CHAN_RECV:
            break;
        }

poll_events:
        LOG_INFO(
            "queues_swapped=%d, coro=%p, sched->io_q.first=%p\n",
            queues_swapped,
            (void *)coro,
            (void *)sched->io_q.first
        );
        if ((queues_swapped || coro == NULL) && sched->io_q.first != NULL) {
            LOG_INFO("[id=%" PRIuMAX "] epoll_wait\n", sched - sched->ctx->scheds_items);
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
            // continue;
        }

        if (global_queue_ready) {
            LOG_INFO("[id=%" PRIuMAX "] FETCH_BATCH\n", sched - sched->ctx->scheds_items);
            size_t count = 0;
            art_coro_gqueue_lock(&sched->ctx->global_q);
            for (;;) {
                count = art_coro_gqueue_fetch_batch(
                    &sched->ctx->global_q,
                    buffer_q,
                    PIPE_BATCH_SIZE
                );
                LOG_INFO(
                    "[id=%" PRIuMAX "] count=%" PRIuMAX
                    ", io_q.first=%p, buffer_q.first=%p\n",
                    sched - sched->ctx->scheds_items,
                    // (void *)coro,
                    count,
                    (void *)sched->io_q.first,
                    (void *)buffer_q->first
                );
                if (
                    sched->working
                    && count == 0
                    && sched->io_q.first == NULL
                    && sched->recv_l.first == NULL
                    && active_q->first == NULL
                    && buffer_q->first == NULL
                ) {
                    sched->working = false;
                    atomic_fetch_sub(&sched->ctx->n_active_sched, 1);
                    // We manually trigger other schedulers that might have
                    // gone to sleep waiting for new tasks. Maybe we don't need
                    // this, idk
                    pthread_cond_broadcast(&sched->ctx->global_q.cond);
                    break;
                }

                if (
                    coro == NULL
                    && count == 0
                    && sched->io_q.first == NULL
                    && buffer_q->first == NULL
                    // && sched->recv_l.first == NULL
                ) {
                    LOG_INFO("[id=%" PRIuMAX "] WAIT\n", sched - sched->ctx->scheds_items);
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
        ARTCoro *coro = sched->recv_l.first;
        coro != NULL;
        coro = DOUBLE_LIST_NEXT(coro,)
    ) {
        art_coro_cleanup(coro);
    }
    art_coro_deque_cleanup(&sched->recv_l);

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
