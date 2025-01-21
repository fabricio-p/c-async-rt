#include <stdio.h>
#include <inttypes.h>
#include "async.h"

DEFINE_CORO(printer1) {
    CORO_INIT_BEGIN(int, int, int);
    *coro_state = *coro_argument;
    CORO_INIT_END();

    CORO_STAGE(1);
    printf("stage(%d) 1\n", *coro_state);
    CORO_YIELD(2);

    CORO_STAGE(2);
    printf("stage(%d) 2\n", *coro_state);

    CORO_END();
}

typedef struct {
    Coroutine c1;
} Printer2State;

DEFINE_CORO(printer2) {
    CORO_INIT_BEGIN(int, Printer2State, void);
    CORO_INIT_END();

    CORO_STAGE(1);
    printf("sigma 1\n");
    CORO_YIELD(2);

    CORO_STAGE(2);
    printf("sigma 2\n");

    CORO_STAGE(3);
    CORO_RUN(&coro_state->c1, printer1, &(int) { 1 });
    printf("sigma 3\n");
    CORO_AWAIT_BEGIN(4, &coro_state->c1);
    coro_cleanup(&coro_state->c1);
    printf("sigma 4\n");

    CORO_END();
}

int main() {
    CoroutineQueue queue;
    coro_queue_init(&queue);

    Coroutine coro1, coro2;
    coro_init(&coro1, printer1, &(int) { 0 });
    coro_init(&coro2, printer2, NULL);
    coro_queue_push_coro(&queue, &coro1);
    coro_queue_push_coro(&queue, &coro2);
    while (!coro_queue_is_empty(&queue)) {
        CoroutineQueue_Node *node = coro_queue_pop(&queue);
        if (coro_run(node->coro, &queue) == CORO_DONE) {
            coro_queue_node_delete(node);
        } else {
            coro_queue_push(&queue, node);
        }
    }

    free(coro1.ctx.ret); coro_cleanup(&coro1);
    free(coro2.ctx.ret); coro_cleanup(&coro2);
    coro_queue_cleanup(&queue);
    return 0;
}