#include <string.h>

#include "chan.h"

void
art_chan_init(
    ARTChan *chan,
    size_t item_size,
    ssize_t size,
    OpaqueMemory buffer
) {
    art_coro_deque_init(&chan->receivers);
    chan->item_size = item_size;
    if (buffer == NULL) {
        chan->buffer = malloc(size * item_size);
        chan->size = size;
    } else {
        chan->buffer = buffer;
        chan->size = -size;
    }
    chan->head = 0;
    chan->tail = 0;
}

bool
art_chan_push(ARTChan *chan, void const *src) {
    (void)chan;
    (void)src;
    return false;
}

void
art_chan_cleanup(ARTChan *chan) {
    art_coro_deque_cleanup(&chan->receivers);
    if (chan->size > 0) {
        free(chan->buffer);
    }
    memset(chan, 0, sizeof(*chan));
}
