#include <stdio.h>
#include "coro.h"
#include "async.h"
#include "logging.h"

static atomic_uintmax_t counter = 0;

void
art_coro_state_init(ARTCoroState *ctx) {
    ctx->data = ctx->data_size != 0 ? malloc(ctx->data_size) : NULL;
}

void
art_coro_init(ARTCoro *coro, ARTCoroFunctionPtr fn, void *arg, size_t flags) {
    coro->id = atomic_fetch_add_explicit(&counter, 1, memory_order_relaxed);
    coro->prev = NULL;
    coro->next = NULL;
    coro->fn = fn;
    coro->flags = flags;
    coro->stage = 0;
    coro->state.data = NULL;
    coro->state.data_size = 0;
    ARTCoroResult res = fn(NULL, &coro->state, arg, 0);
    coro->status = res.status;
    coro->stage = res.stage;
    LOG_INFO("Coroutine (%" PRIuMAX ") initialized\n", coro->id);
}

void
art_coro_cleanup(ARTCoro *coro) {
    if (coro->state.data != NULL) {
        free(coro->state.data);
        coro->state.data = NULL;
    }
    LOG_INFO("ARTCoro (%" PRIuMAX ") cleaned up\n", coro->id);
}

ARTCoroResult
art_coro_run(ARTScheduler *scheduler, ARTCoro *coro) {
    ARTCoroResult res = coro->fn(scheduler, &coro->state, NULL, coro->stage);
    coro->status = res.status;
    coro->stage = res.stage;
    return res;
}

