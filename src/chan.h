#ifndef ART_CHAN_H
#define ART_CHAN_H
#include <pthread.h>

#include "ds.h"
#include "queues.h"

typedef struct art_chan_coro_t {
    DOUBLE_LIST_PARTS(, struct art_chan_receiver_t);
    ARTCoro *coro;
    struct art_scheduler_t *owner_sched;

    void *storage;
} ARTChanCoro;

typedef struct art_chan_t {
    DEQUE_PARTS(recvs_, ARTChanCoro);
    DEQUE_PARTS(sends_, ARTChanCoro);
    pthread_spinlock_t recvs_lock;
    pthread_spinlock_t sends_lock;

    size_t item_size;
    OpaqueMemory buffer;
    size_t size;
    // atomic_uint_fast32_t head;
    // atomic_uint_fast32_t head_ready;
    // atomic_uint_fast32_t tail;
    // atomic_uint_fast32_t tail_done;
    uint_fast32_t head;
    uint_fast32_t tail;
    pthread_mutex_t mtx;

    bool external : 1;
} ARTChan;

void
art_chan_init(
    ARTChan *chan,
    size_t item_size,
    ssize_t size,
    OpaqueMemory buffer
);
bool
art_chan_push(ARTChan *chan, void const *src, ARTChanCoro *out);
bool
art_chan_pop(ARTChan *chan, void *dst);
void
art_chan_cleanup(ARTChan *chan);

#endif /* ART_CHAN_H */
