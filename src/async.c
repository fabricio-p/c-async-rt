#include <stdatomic.h>
#include <stdio.h>

#include "async.h"

static atomic_uintmax_t counter = 0;

void
coro_context_init(CoroutineContext *ctx) {
    ctx->ret = ctx->r_size != 0 ? malloc(ctx->r_size) : NULL;
    ctx->state = ctx->state_size != 0 ? malloc(ctx->state_size) : NULL;
}

// void
// coro_context_cleanup(CoroutineContext *ctx) {
//     // free(ctx->ret);
//     free(ctx->state);
// }

#define LOG_INFO_(fmtstr)                                               \
    fprintf(stderr, "[INFO:" __FILE__ ":%d] " fmtstr, __LINE__)

#define LOG_INFO(fmtstr, ...)                                               \
    fprintf(stderr, "[INFO:" __FILE__ ":%d] " fmtstr, __LINE__, __VA_ARGS__)

void
coro_init(Coroutine *coro, CoroutineFunctionPtr fn, void *arg) {
    coro->id = counter++;
    coro->fn = fn;
    coro->stage = 0;
    coro->ctx.state = NULL;
    coro->ctx.ret = NULL;
    coro->ctx.r_size = 0;
    coro->ctx.state_size = 0;
    CoroutineResult res = fn(&coro->ctx, NULL, arg, 0);
    coro->state = res.state;
    coro->stage = res.stage;
    LOG_INFO("Coroutine (%" PRIuMAX ") initialized\n", coro->id);
}

void
coro_cleanup(Coroutine *coro) {
    if (coro->ctx.state != NULL) {
        free(coro->ctx.state);
        coro->ctx.state = NULL;
    }
    if (coro->ctx.ret != NULL) {
        free(coro->ctx.ret);
        coro->ctx.ret = NULL;
    }
    LOG_INFO("Coroutine (%" PRIuMAX ") cleaned up\n", coro->id);
}

CoroutineState
coro_run(Coroutine *coro, CoroutineQueue *queue) {
    if (coro->state == CORO_DONE) {
        if (coro->ctx.state != NULL) {
            free(coro->ctx.state);
            coro->ctx.state = NULL;
        }
        return CORO_DONE;
    }
    CoroutineResult res = coro->fn(&coro->ctx, queue, NULL, coro->stage);
    coro->state = res.state;
    coro->stage = res.stage;
    return res.state;
}

CoroutineQueue_Node *
coro_queue_node_new(
    Coroutine *coro,
    size_t flags
) {
    CoroutineQueue_Node *node = malloc(sizeof(CoroutineQueue_Node));
    node->coro = coro;
    node->next = NULL;
    node->flags = flags;
    return node;
}

void
coro_queue_node_delete(CoroutineQueue_Node *node) {
    if (node->flags & (1 << CORO_DISCARD)) {
        coro_cleanup(node->coro);
    }
    if (node->flags & (1 << CORO_FREE)) {
        free(node->coro);
    }
    node->coro = NULL;
    free(node);
}

void
coro_queue_init(CoroutineQueue *queue) {
    queue->head = NULL;
    queue->tail = NULL;
}

void
coro_queue_push_coro(
    CoroutineQueue *queue,
    Coroutine *coro,
    size_t flags
) {
    CoroutineQueue_Node *node = coro_queue_node_new(coro, flags);
    coro_queue_push(queue, node);
}

void
coro_queue_push(CoroutineQueue *queue, CoroutineQueue_Node *node) {
    if (queue->head == NULL) {
        queue->head = node;
        queue->tail = node;
    } else {
        queue->tail->next = node;
        queue->tail = node;
    }
}

CoroutineQueue_Node *
coro_queue_pop(CoroutineQueue *queue) {
    CoroutineQueue_Node *node = queue->head;
    if (node != NULL) {
        queue->head = node->next;
    }
    if (queue->head == NULL) {
        queue->tail = NULL;
    }
    node->next = NULL;
    return node;
}

void
coro_queue_cleanup(CoroutineQueue *queue) {
    while (queue->head != NULL) {
        CoroutineQueue_Node *node = coro_queue_pop(queue);
        coro_queue_node_delete(node);
    }
}
