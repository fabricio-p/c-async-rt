#ifndef ART_CO_H
#define ART_CO_H

#include <stdlib.h>
#include <pthread.h>
#include <inttypes.h>
#include "ds.h"

typedef void *OpaqueMemory;
typedef struct art_context_t ARTContext;
typedef struct art_scheduler_id_t ARTSchedulerID;

typedef enum coroutine_status_t {
    ART_CO_DONE,
    ART_CO_INITIALIZED,
    ART_CO_WAITING,
    ART_CO_WAITING_IO,
} ARTCoroStatus;

typedef struct coroutine_result_t {
    ARTCoroStatus status;
    uint64_t stage;
} ARTCoroResult;

typedef struct coroutine_state_t {
    size_t r_size;
    size_t data_size;
    OpaqueMemory ret;
    OpaqueMemory data;
} ARTCoroState;

typedef ARTCoroResult (*ARTCoroFunctionPtr)(
    ARTContext *ctx,
    ARTCoroState *state,
    OpaqueMemory arg,
    uint64_t stage
);

typedef struct coroutine_t {
    DOUBLE_LIST_PARTS(, struct coroutine_t);
    size_t flags;

    ARTCoroFunctionPtr fn;
    uint64_t id;
    uint64_t stage;
    uint32_t status;
    ARTCoroState state;
} ARTCoro;

struct art_scheduler_id_t {
    size_t index;
};

typedef struct art_coro_deque_t {
    pthread_mutex_t mtx;
    // pthread_cond_t cond;

    ARTCoro *first;
    ARTCoro *last;
} ARTCoroDeque;

#define ART_DEFAULT_CORO_DEQUE      \
    (ARTCoroDeque) {                \
        PTHREAD_MUTEX_INITIALIZER,  \
        NULL,                       \
        NULL                        \
    }

typedef struct art_scheduler_t {
    pthread_t thread_id;
    ARTCoroDeque io_q;
    ARTCoroDeque wait_q;
    ARTCoroDeque active_q;

    int epoll_fd;
    DARRAY_PARTS(epoll_evs_, struct epoll_event);
} ARTScheduler;

typedef struct art_context_settings_t {
    size_t sched_count;
    size_t use_current_thread : 1;
} ARTContextSettings;

typedef struct art_cleanup_entry_t {
    void (*fn)(OpaqueMemory);
    OpaqueMemory data;
} ARTCleanupEntry;

struct art_context_t {
    ARRAY_PARTS(scheds_, ARTScheduler);
    ARTCoroDeque global_q;
    DARRAY_PARTS(cleanups_, ARTCleanupEntry);
};

enum {
    ART_CO_DISCARD,
    ART_CO_FREE
};

#define ART_DEFINE_CO(name)                                                 \
    ARTCoroResult                                                           \
    name(                                                                   \
        __attribute__((unused)) ARTContext *_coro_ctx_,                     \
        __attribute__((unused)) ARTCoroState *_coro_state_,                 \
        __attribute__((unused)) OpaqueMemory _coro_arg_,                    \
        __attribute__((unused)) uint64_t stage                              \
    )

#define _ART_CO_CASE_(c) case (c):

// TODO: Better allocation bs
#define ART_CO_INIT_BEGIN(r_type, data_type, arg_type)                      \
    __attribute__((unused)) r_type *coro_result = _coro_state_->ret;        \
    __attribute__((unused)) data_type *coro_data =                          \
        (data_type *)_coro_state_->data;                                    \
    switch (stage) {                                                        \
    _ART_CO_CASE_(0) {                                                      \
        _coro_state_->r_size = sizeof(r_type);                              \
        _coro_state_->data_size = sizeof(data_type);                        \
        art_coro_state_init(_coro_state_);                                  \
        coro_result = (r_type *)_coro_state_->ret;                          \
        coro_data = (data_type *)_coro_state_->data;                        \
        __attribute__((unused)) arg_type *coro_argument =                   \
            (arg_type *)_coro_arg_

#define ART_CO_INIT_END()                                                   \
    return (ARTCoroResult) {                                                \
        .status = ART_CO_INITIALIZED,                                       \
        .stage = ++stage                                                    \
    };

#define ART_CO_CUT_STAGE(stage_) } break; _ART_CO_CASE_(stage_) {

#define ART_CO_STAGE(stage_)                                                  \
    } __attribute__((fallthrough)); _ART_CO_CASE_(stage_) {

#define ART_CO_YIELD(stage_)                                                \
    return (ARTCoroResult) {                                                \
        .status = ART_CO_WAITING,                                           \
        .stage = stage_                                                     \
    };

#define ART_CO_RETURN(stage_)                                               \
    return (ARTCoroResult) {                                                \
        .status = ART_CO_DONE,                                              \
        .stage = stage_                                                     \
    };

#define ART_CO_DONE(stage_)                                                 \
    return (ARTCoroResult) {                                                \
        .status = ART_CO_DONE,                                              \
        .stage = stage_                                                     \
    };

#define ART_CO_END()                                                        \
    } break; } ART_CO_RETURN(0)

#define ART_CO_EMPTY() ART_CO_DONE(0)

#define ART_CO_RUN(coro_, fn, arg_)                                         \
    coro_init(coro_, fn, arg_);                                             \
    coro_queue_push_coro(_coro_queue_, coro_)

#define ART_CO_AWAIT(stage_, coro_)                                         \
    ART_CO_STAGE(stage_);                                                   \
    if ((coro_)->state != ART_CO_DONE) { ART_CO_YIELD((stage_)); }

void art_coro_state_init(ARTCoroState *ctx);
// void coro_context_cleanup(ARTCoroContext *ctx);

void art_coro_init(ARTCoro *coro, ARTCoroFunctionPtr fn, void *arg, size_t flags);
ARTCoroStatus coro_run(ARTContext *ctx, ARTCoro *coro);
void art_coro_cleanup(ARTCoro *coro);

void art_scheduler_init(ARTScheduler *sched);
void art_scheduler_start(ARTScheduler *sched);
void art_scheduler_cancel(ARTScheduler *sched);
void art_scheduler_cleanup(ARTScheduler *sched);

// TODO: Use some enum for statuses or sum shite

int art_context_init(ARTContext *ctx, ARTContextSettings const *settings);
ARTCoro *art_context_new_coro(ARTContext *ctx);
void art_context_run_coro(ARTContext *ctx, ARTCoro *coro);
void art_context_register_cleanup(
    ARTContext *ctx,
    void (*fn)(OpaqueMemory),
    OpaqueMemory data
);
void art_context_join(ARTContext *ctx);
void art_context_cleanup(ARTContext *ctx);

#endif /* ART_CO_H */
