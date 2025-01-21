#ifndef CORO_H
#define CORO_H

#include <stdlib.h>
#include <inttypes.h>

typedef void *OpaqueMemory;
typedef struct coroutine_queue_t CoroutineQueue;

typedef enum coroutine_state_t {
    CORO_DONE,
    CORO_INITIALIZED,
    CORO_WAITING,
    CORO_WAITING_IO,
} CoroutineState;

typedef struct coroutine_result_t {
    CoroutineState state;
    uint64_t stage;
} CoroutineResult;

typedef struct coroutine_context_t {
    size_t r_size;
    size_t state_size;
    OpaqueMemory ret;
    OpaqueMemory state;
} CoroutineContext;

typedef CoroutineResult (*CoroutineFunctionPtr)(
    CoroutineContext *ctx,
    CoroutineQueue *queue,
    OpaqueMemory arg,
    uint64_t stage
);

typedef struct coroutine_t {
    CoroutineFunctionPtr fn;
    uint64_t id;
    uint64_t stage;
    uintptr_t state;
    CoroutineContext ctx;
} Coroutine;

typedef struct coroutine_queue_node_t {
    Coroutine *coro;
    struct coroutine_queue_node_t *next;
} CoroutineQueue_Node;

struct coroutine_queue_t {
    CoroutineQueue_Node *head;
    CoroutineQueue_Node *tail;
};

#define DEFINE_CORO(name)                                                   \
    CoroutineResult name(                                                   \
        __attribute__((unused)) CoroutineContext *_coro_ctx_,               \
        __attribute__((unused)) CoroutineQueue *_coro_queue_,               \
        __attribute__((unused)) OpaqueMemory _coro_arg_,                    \
        __attribute__((unused)) uint64_t stage                              \
    )

#define _CORO_CASE_(c) case (c):

// TODO: Better allocation bs
#define CORO_INIT_BEGIN(r_type, state_type, arg_type)                       \
    __attribute__((unused)) r_type *coro_result = _coro_ctx_->ret;          \
    __attribute__((unused)) state_type *coro_state =                        \
        (state_type *)_coro_ctx_->state;                                    \
    switch (stage) {                                                        \
    _CORO_CASE_(0) {                                                        \
        _coro_ctx_->r_size = sizeof(r_type);                                \
        _coro_ctx_->state_size = sizeof(state_type);                        \
        coro_context_init(_coro_ctx_);                                      \
        coro_result = (r_type *)_coro_ctx_->ret;                            \
        coro_state = (state_type *)_coro_ctx_->state;                       \
        __attribute__((unused)) arg_type *coro_argument =                   \
            (arg_type *)_coro_arg_

#define CORO_INIT_END()                                                     \
    return (CoroutineResult) {                                              \
        .state = CORO_INITIALIZED,                                          \
        .stage = ++stage                                                    \
    };

#define CORO_CUT_STAGE(stage_) } break; _CORO_CASE_(stage_) {

#define CORO_STAGE(stage_) } __attribute__((fallthrough)); _CORO_CASE_(stage_) {

#define CORO_YIELD(stage_)                                                  \
    return (CoroutineResult) {                                              \
        .state = CORO_WAITING,                                              \
        .stage = stage_                                                     \
    };

#define CORO_RETURN(stage_)                                                 \
    return (CoroutineResult) {                                              \
        .state = CORO_DONE,                                                 \
        .stage = stage_                                                     \
    };

#define CORO_DONE(stage_)                                                   \
    return (CoroutineResult) {                                              \
        .state = CORO_DONE,                                                 \
        .stage = stage_                                                     \
    };

#define CORO_END()                                                          \
    } break; } CORO_RETURN(0)

#define CORO_EMPTY()                                                        \
    return (CoroutineResult) { .state = CORO_DONE, .stage = 0 }

#define CORO_RUN(coro_, fn, arg_)                                           \
    coro_init(coro_, fn, arg_);                                             \
    coro_queue_push_coro(_coro_queue_, coro_)

#define CORO_AWAIT_BEGIN(stage_, coro_)                                     \
    CORO_STAGE(stage_);                                                     \
    if ((coro_)->state != CORO_DONE) { CORO_YIELD((stage_)); }

void coro_context_init(CoroutineContext *ctx);
void coro_context_cleanup(CoroutineContext *ctx);

void coro_init(Coroutine *coro, CoroutineFunctionPtr fn, void *arg);
CoroutineState coro_run(Coroutine *coro, CoroutineQueue *queue);
void coro_cleanup(Coroutine *coro);


CoroutineQueue_Node *coro_queue_node_new(Coroutine *coro);
void coro_queue_node_delete(CoroutineQueue_Node *coro);

void coro_queue_init(CoroutineQueue *queue);
void coro_queue_push_coro(CoroutineQueue *queue, Coroutine *coro);
void coro_queue_push(CoroutineQueue *queue, CoroutineQueue_Node *node);
CoroutineQueue_Node *coro_queue_pop(CoroutineQueue *queue);

static inline int coro_queue_is_empty(CoroutineQueue *queue) {
    return queue->head == NULL;
}

void coro_queue_cleanup(CoroutineQueue *queue);

#endif /* CORO_H */