#include <stdio.h>
#include "queues.h"
#include "logging.h"

static inline void
art_coro_push_single(ARTCoroGQueue *gqueue, ARTCoro *coro);

void
art_coro_gqueue_init(ARTCoroGQueue *gqueue) {
    pthread_mutex_init(&gqueue->mtx, NULL);
    pthread_cond_init(&gqueue->cond, NULL);
    gqueue->eventfd = eventfd(0, EFD_SEMAPHORE);
    gqueue->first = NULL;
    gqueue->last = NULL;
}

void
art_coro_gqueue_push(ARTCoroGQueue *gqueue, ARTCoro **coros, size_t n) {
    pthread_mutex_lock(&gqueue->mtx);
    for (size_t i = 0; i < n; i++) {
        art_coro_push_single(gqueue, coros[i]);
    }
    // eventfd_write(gqueue->eventfd, 4);
    // LOG_INFO_("WROTE TO EVENTFD MFFFFF\n");
    pthread_mutex_unlock(&gqueue->mtx);
    pthread_cond_broadcast(&gqueue->cond);
}


ARTCoro *
art_coro_gqueue_pop(ARTCoroGQueue *gqueue) {
    ARTCoro *coro = NULL;
    if (gqueue->first == NULL) {
        goto end;
    } else {
        coro = gqueue->first;
        gqueue->first = coro->next;
        if (gqueue->first == NULL) {
            gqueue->last = NULL;
        }
        coro->next = NULL;
    }
end:
    return coro;
}

size_t
art_coro_gqueue_fetch_batch(
    ARTCoroGQueue *queue,
    ARTCoroDeque *deque,
    size_t n_max
) {
    size_t count = 0;
    for (size_t i = 0; i < n_max; i++, count++) {
        ARTCoro *coro = art_coro_gqueue_pop(queue);
        if (coro == NULL) {
            break;
        }
        art_coro_deque_push_back_lock(deque, coro);
    }
    return count;
}

void
art_coro_gqueue_lock(ARTCoroGQueue *gqueue) {
    pthread_mutex_lock(&gqueue->mtx);
}

void
art_coro_gqueue_unlock(ARTCoroGQueue *gqueue) {
    pthread_mutex_unlock(&gqueue->mtx);
}

void
art_coro_gqueue_wait(ARTCoroGQueue *gqueue) {
    pthread_cond_wait(&gqueue->cond, &gqueue->mtx);
}

void
art_coro_gqueue_cleanup(ARTCoroGQueue *gqueue) {
    pthread_mutex_destroy(&gqueue->mtx);
    pthread_cond_destroy(&gqueue->cond);
    gqueue->first = NULL;
    gqueue->last = NULL;
}

static inline void
art_coro_push_single(ARTCoroGQueue *gqueue, ARTCoro *coro) {
    if (gqueue->first == NULL) {
        gqueue->first = coro;
        gqueue->last = coro;
    } else {
        gqueue->last->next = coro;
        coro->prev = gqueue->last;
        gqueue->last = coro;
    }
}

void
art_coro_deque_init(ARTCoroDeque *deque) {
    pthread_spin_init(&deque->lock, PTHREAD_PROCESS_PRIVATE);
    deque->first = NULL;
    deque->last = NULL;
}

void
art_coro_deque_push_back(ARTCoroDeque *deque, ARTCoro *coro) {
    coro->prev = deque->last;
    coro->next = NULL;
    if (deque->last != NULL) {
        deque->last->next = coro;
    } else {
        deque->first = coro;
    }
    deque->last = coro;
}

void
art_coro_deque_push_back_lock(ARTCoroDeque *deque, ARTCoro *coro) {
    pthread_spin_lock(&deque->lock);
    art_coro_deque_push_back(deque, coro);
    pthread_spin_unlock(&deque->lock);
}

ARTCoro *
art_coro_deque_pop_front(ARTCoroDeque *deque) {
    ARTCoro *coro = NULL;
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
    return coro;
}

ARTCoro *
art_coro_deque_pop_front_lock(ARTCoroDeque *deque) {
    pthread_spin_lock(&deque->lock);
    ARTCoro *coro = art_coro_deque_pop_front(deque);
    pthread_spin_unlock(&deque->lock);
    return coro;
}

void
art_coro_deque_pluck(ARTCoroDeque *deque, ARTCoro *coro) {
    if (coro->prev == NULL) {
        deque->first = coro->next;
    } else {
        coro->prev->next = coro->next;
    }
    if (coro->next == NULL) {
        deque->last = coro->prev;
    } else {
        coro->next->prev = coro->prev;
    }
    coro->prev = NULL;
    coro->next = NULL;
}

void
art_coro_deque_cleanup(ARTCoroDeque *deque) {
    pthread_spin_destroy(&deque->lock);
    deque->first = NULL;
    deque->last = NULL;
}
