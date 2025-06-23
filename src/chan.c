#include <string.h>

#include "chan.h"

static bool
art_chan_full(ARTChan *chan) {
    return (chan->head + 1) % chan->size == chan->tail;
}

static bool
art_chan_empty(ARTChan *chan) {
    return chan->head == chan->tail;
}

void
art_chan_init(
    ARTChan *chan,
    size_t item_size,
    ssize_t size,
    OpaqueMemory buffer
) {
    chan->recvs_first = NULL;
    chan->recvs_last = NULL;
    chan->sends_first = NULL;
    chan->sends_last = NULL;
    pthread_spin_init(&chan->recvs_lock, PTHREAD_PROCESS_PRIVATE);
    pthread_spin_init(&chan->sends_lock, PTHREAD_PROCESS_PRIVATE);
    chan->item_size = item_size;
    chan->size = size;
    if (buffer == NULL && size != 0) {
        chan->buffer = malloc(size * item_size);
        chan->external = false;
    } else {
        chan->buffer = buffer;
        chan->external = true;
    }
    chan->head = 0;
    chan->tail = 0;
    pthread_mutex_init(&chan->mtx, NULL);
}

bool
art_chan_push_buffer(ARTChan *chan, void const *src) {
    bool result = false;
    pthread_mutex_lock(&chan->mtx);
    if (chan->size == 0 || art_chan_full(chan)) {
        goto end;
    }
    memcpy(
        (uint8_t *)chan->buffer + chan->head * chan->item_size,
        src,
        chan->item_size
    );
    chan->head = (chan->head + 1) % chan->size;
    result = true;
end:
    pthread_mutex_unlock(&chan->mtx);
    return result;
}

bool
art_chan_push(ARTChan *chan, void const *src, ARTChanCoro *out) {
    pthread_spin_lock(&chan->recvs_lock);
    ARTChanCoro *receiver = DEQUE_POP_FRONT(chan, recvs_, , ARTChanCoro);
    pthread_spin_unlock(&chan->recvs_lock);
    if (receiver == NULL) {
        return art_chan_push_buffer(chan, src);
    }
    memcpy(out, receiver, sizeof(ARTChanCoro));
    memcpy(out->storage, src, chan->item_size);
    return true;
}

bool
art_chan_pop(ARTChan *chan, void *dst) {
    bool result = false;
    pthread_mutex_lock(&chan->mtx);
    if (chan->size == 0 || art_chan_empty(chan)) {
        goto end;
    }
    memcpy(
        dst,
        (uint8_t const *)chan->buffer + chan->tail * chan->item_size,
        chan->item_size
    );
    chan->tail = (chan->tail + 1) % chan->size;
    result = true;
end:
    pthread_mutex_unlock(&chan->mtx);
    return result;
}

void
art_chan_cleanup(ARTChan *chan) {
    chan->recvs_first = NULL;
    chan->recvs_last = NULL;
    chan->sends_first = NULL;
    chan->sends_last = NULL;
    pthread_spin_destroy(&chan->recvs_lock);
    pthread_spin_destroy(&chan->sends_lock);
    if (!chan->external) {
        free(chan->buffer);
    }
    chan->item_size = 0;
    chan->buffer = NULL;
    chan->size = 0;
    chan->head = 0;
    chan->tail = 0;
    pthread_mutex_destroy(&chan->mtx);
}
