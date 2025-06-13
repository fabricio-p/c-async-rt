#include <stdatomic.h>
#include <stdio.h>

#include "async.h"

static atomic_uintmax_t counter = 0;

void
art_scheduler_loop(ARTScheduler *sched);

void
coro_state_init(ARTCoroState *ctx) {
    ctx->ret = ctx->r_size != 0 ? malloc(ctx->r_size) : NULL;
    ctx->data = ctx->data_size != 0 ? malloc(ctx->data_size) : NULL;
}

// void
// coro_context_cleanup(ARTCoroContext *ctx) {
//     // free(ctx->ret);
//     free(ctx->state);
// }

#define LOG_INFO_(fmtstr)                                               \
    fprintf(stderr, "[INFO:" __FILE__ ":%d] " fmtstr, __LINE__)

#define LOG_INFO(fmtstr, ...)                                               \
    fprintf(stderr, "[INFO:" __FILE__ ":%d] " fmtstr, __LINE__, __VA_ARGS__)

void
art_coro_init(ARTCoro *coro, ARTCoroFunctionPtr fn, void *arg, size_t flags) {
    coro->id = atomic_fetch_add_explicit(&counter, 1, memory_order_relaxed);
    coro->fn = fn;
    coro->flags = flags;
    coro->stage = 0;
    coro->state.data = NULL;
    coro->state.ret = NULL;
    coro->state.r_size = 0;
    coro->state.data_size = 0;
    ARTCoroResult res = fn(NULL, &coro->state, arg, 0);
    coro->status = res.status;
    coro->stage = res.stage;
    LOG_INFO("ARTCoro (%" PRIuMAX ") initialized\n", coro->id);
}

void
art_coro_cleanup(ARTCoro *coro) {
    if (coro->state.data != NULL) {
        free(coro->state.data);
        coro->state.data = NULL;
    }
    if (coro->state.ret != NULL) {
        free(coro->state.ret);
        coro->state.ret = NULL;
    }
    LOG_INFO("ARTCoro (%" PRIuMAX ") cleaned up\n", coro->id);
}

ARTCoroStatus
art_coro_run(ARTContext *ctx, ARTCoro *coro) {
    if (coro->status == ART_CO_DONE) {
        if (coro->state.data != NULL) {
            free(coro->state.data);
            coro->state.data = NULL;
        }
        return ART_CO_DONE;
    }
    ARTCoroResult res = coro->fn(ctx, &coro->state, NULL, coro->stage);
    coro->status = res.status;
    coro->stage = res.stage;
    return res.status;
}

int
art_context_init(ARTContext *ctx, ARTContextSettings const *settings) {
    ctx->scheds_size = settings->sched_count;
    ctx->scheds_items = calloc(ctx->scheds_size, sizeof(ARTCoroDeque));
    for (size_t i = 0; i < ctx->scheds_size; i++) {
        art_scheduler_init(&ctx->scheds_items[i]);
        if (i != 0 || !settings->use_current_thread) {
            art_scheduler_start(&ctx->scheds_items[i]);
        }
    }
    if (settings->use_current_thread) {
        ctx->scheds_items[0].thread_id = pthread_self();
    }
    ctx->global_q = ART_DEFAULT_CORO_DEQUE;

    return 0;
}

ARTCoro *
art_context_new_coro(__attribute__((unused)) ARTContext *ctx) {
    ARTCoro *coro = malloc(sizeof(ARTCoro));
    return coro;
}

void
art_coro_deque_push_back(ARTCoroDeque *deque, ARTCoro *coro) {
    pthread_mutex_lock(&deque->mtx);

    coro->prev = deque->last;
    coro->next = NULL;
    if (deque->last != NULL) {
        deque->last->next = coro;
    } else {
        deque->first = coro;
    }
    deque->last = coro;

    pthread_mutex_unlock(&deque->mtx);
}

ARTCoro *
art_coro_deque_pop_front(ARTCoroDeque *deque) {
    ARTCoro *coro = NULL;

    pthread_mutex_lock(&deque->mtx);

    if (deque->first == NULL) {
        goto end;
    }
    coro = deque->first;
    if (deque->last == coro) {
        deque->last = NULL;
    }
    else {
        coro->next->prev = coro->prev;
    }
    deque->first = coro->next;
    coro->next = NULL;

end:
    pthread_mutex_unlock(&deque->mtx);
    return coro;
}

void
art_context_run_coro(ARTContext *ctx, ARTCoro *coro) {
    art_coro_deque_push_back(&ctx->global_q, coro);
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
    if (pthread_equal(pthread_self(), ctx->scheds_items[0].thread_id)) {
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
}
