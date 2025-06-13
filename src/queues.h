#ifndef ART_QUEUES_H
#define ART_QUEUES_H
#include <pthread.h>
#include "coro.h"

typedef struct art_coro_gqueue_t {
    ARTCoro *first;
    ARTCoro *last;

    pthread_mutex_t mtx;
    pthread_cond_t cond;
} ARTCoroGQueue;

typedef struct art_coro_deque_t {
    pthread_spinlock_t lock;
    ARTCoro *first;
    ARTCoro *last;
} ARTCoroDeque;

void
art_coro_gqueue_init(ARTCoroGQueue *gqueue);
void
art_coro_gqueue_push(ARTCoroGQueue *gqueue, ARTCoro **coros, size_t n);
ARTCoro *
art_coro_gqueue_pop(ARTCoroGQueue *gqueue);
size_t
art_coro_gqueue_fetch_batch(ARTCoroGQueue *src, ARTCoroDeque *dst, size_t n_max);
void
art_coro_gqueue_lock(ARTCoroGQueue *gqueue);
void
art_coro_gqueue_unlock(ARTCoroGQueue *gqueue);
void
art_coro_gqueue_wait(ARTCoroGQueue *gqueue);
void
art_coro_gqueue_cleanup(ARTCoroGQueue *gqueue);

void
art_coro_deque_init(ARTCoroDeque *deque);
void
art_coro_deque_push_back(ARTCoroDeque *deque, ARTCoro *coro);
void
art_coro_deque_push_back_lock(ARTCoroDeque *deque, ARTCoro *coro);
ARTCoro *
art_coro_deque_pop_front(ARTCoroDeque *deque);
ARTCoro *
art_coro_deque_pop_front_lock(ARTCoroDeque *deque);
void
art_coro_deque_cleanup(ARTCoroDeque *deque);

#endif /* ART_QUEUES_H */
