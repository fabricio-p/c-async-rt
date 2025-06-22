#ifndef ART_CHAN_H
#define ART_CHAN_H
#include <pthread.h>

#include "ds.h"
#include "queues.h"

typedef struct art_chan_t {
    ARTCoroDeque receivers;
    size_t item_size;
    OpaqueMemory buffer;
    ssize_t size; // negative means external buffer
    atomic_uint head;
    atomic_uint tail;
} ARTChan;

void
art_chan_init(
    ARTChan *chan,
    size_t item_size,
    ssize_t size,
    OpaqueMemory buffer
);
bool
art_chan_push(ARTChan *chan, void const *src);
void
art_chan_pop(ARTChan *chan, void *dst);
void
art_chan_cleanup(ARTChan *chan);

#endif /* ART_CHAN_H */
