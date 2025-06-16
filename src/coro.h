#ifndef ART_CORO_H
#define ART_CORO_H
#include <stdbool.h>
#include "ds.h"

struct art_scheduler_t;

typedef enum coroutine_status_t {
    ART_CO_DONE,
    ART_CO_INITIALIZED,
    ART_CO_WAITING,
    ART_CO_POLL_IO_READ,
    ART_CO_POLL_IO_WRITE,
} ARTCoroStatus;

typedef struct coroutine_result_t {
    ARTCoroStatus status;
    uint64_t stage;
    union {
        struct {
            int fd;
            bool once;
        } io;
    } d;
} ARTCoroResult;

typedef struct coroutine_state_t {
    size_t r_size;
    size_t data_size;
    OpaqueMemory ret;
    OpaqueMemory data;
} ARTCoroState;

typedef ARTCoroResult (*ARTCoroFunctionPtr)(
    struct art_scheduler_t *scheduler,
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

enum {
    ART_CO_DISCARD,
    ART_CO_FREE
};

#define ART_DEFINE_CO(name)                                                 \
    ARTCoroResult                                                           \
    name(                                                                   \
        __attribute__((unused)) ARTScheduler *_coro_scheduler_,             \
        __attribute__((unused)) ARTCoroState *_coro_state_,                 \
        __attribute__((unused)) OpaqueMemory _coro_arg_,                    \
        __attribute__((unused)) uint64_t _coro_stage_                       \
    )

#define _ART_CO_CASE_(c) case (c):

// TODO: Better allocation bs
#define ART_CO_INIT_BEGIN(r_type, data_type, arg_type)                      \
    __attribute__((unused)) r_type *coro_result = _coro_state_->ret;        \
    __attribute__((unused)) data_type *coro_data =                          \
        (data_type *)_coro_state_->data;                                    \
    switch (_coro_stage_) {                                                 \
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
        .stage = ++_coro_stage_                                             \
    };

#define ART_CO_CUT_STAGE(stage_)                                            \
    } ART_CO_YIELD(_coro_stage_ + 1);                                       \
    _ART_CO_CASE_(stage_) {

#define ART_CO_STAGE(stage_)                                                \
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

#define ART_CO_AWAIT(stage_, coro_)                                         \
    ART_CO_STAGE(stage_);                                                   \
    if ((coro_)->state != ART_CO_DONE) { ART_CO_YIELD((stage_)); }

#define ART_CO_POLL_IO(stage_, action_, ...)                                \
    return (ARTCoroResult) {                                                \
        .status = ART_CO_POLL_IO_##action_,                                 \
        .stage = stage_,                                                    \
        .d.io = { __VA_ARGS__ }                                             \
    };                                                                      \
    ART_CO_STAGE(stage_)

#define ART_CO_POLL_IO_LOOP_BEGIN(stage_, action_, ...)                     \
    ART_CO_STAGE(stage_);                                                   \
    ARTCoroResult res_##stage_ = (ARTCoroResult) {                          \
        .status = ART_CO_POLL_IO_##action_,                                 \
        .stage = stage_,                                                    \
        .d.io = { __VA_ARGS__ }                                             \
    };

#define ART_CO_POLL_IO_LOOP_END(stage_)                                     \
    return res_##stage_

// #define ART_CO_POLL_IO_LOOP_BREAK(stage_, action_, ...)                      \
//     ART_CO_STAGE(stage_);                                                   \
//     ARTCoroResult res_##stage_ = (ARTCoroResult) {                          \
//         .status = ART_CO_POLL_IO_##action_,                                 \
//         .stage = stage_,                                                    \
//         .d.io = { __VA_ARGS__ }                                             \
//     };                                                                      \
//     break

void
art_coro_state_init(ARTCoroState *ctx);

void
art_coro_init(
    ARTCoro *coro,
    ARTCoroFunctionPtr fn,
    void *arg,
    size_t flags
);
ARTCoroResult
art_coro_run(struct art_scheduler_t *ctx, ARTCoro *coro);
void
art_coro_cleanup(ARTCoro *coro);

#endif /* ART_CORO_H */
