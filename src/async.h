#ifndef ART_CO_H
#define ART_CO_H

#include <stdlib.h>
#include <pthread.h>
#include <inttypes.h>
#include <sys/epoll.h>
#include "ds.h"
#include "coro.h"
#include "queues.h"

typedef struct art_context_t ARTContext;
typedef struct art_scheduler_id_t ARTSchedulerID;

struct art_scheduler_id_t {
    size_t index;
};

typedef struct art_scheduler_t {
    ARTContext *ctx;

    pthread_t thread_id;
    ARTCoroDeque io_q;
    ARTCoroDeque wait_q;
    ARTCoroDeque active_qs[2];
    atomic_uint active_q_idx;

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
    DARRAY_PARTS(cleanups_, ARTCleanupEntry);
    pthread_t main_thread_id;

    ARTCoroGQueue global_q;
};

void
art_scheduler_init(ARTContext *ctx, ARTScheduler *sched);
void
art_scheduler_start(ARTScheduler *sched);
void
art_scheduler_cancel(ARTScheduler *sched);
void
art_scheduler_cleanup(ARTScheduler *sched);

// TODO: Use some enum for statuses or sum shite

int
art_context_init(ARTContext *ctx, ARTContextSettings const *settings);
void
art_context_run(ARTContext *ctx);
ARTCoro
*art_context_new_coro(ARTContext *ctx);
void
art_context_run_coros(ARTContext *ctx, ARTCoro **coros, size_t n);
void
art_context_register_cleanup(
    ARTContext *ctx,
    void (*fn)(OpaqueMemory),
    OpaqueMemory data
);
void
art_context_join(ARTContext *ctx);
void
art_context_cleanup(ARTContext *ctx);

#endif /* ART_CO_H */
