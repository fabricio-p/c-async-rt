#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#include "async.h"
#include "err_utils.h"
#include "logging.h"

typedef struct {
    int id;
    struct timespec timer;
} State;

ART_DEFINE_CO(do_thing) {
    ART_CO_INIT_BEGIN(void, State, int); {
        coro_data->id = *coro_argument;
        LOG_INFO("STARTING COROUTINE %d\n", coro_data->id);
        ART_CO_YIELD(1);
    }
    ART_CO_STAGE(1) {
        LOG_INFO("RUNNING COROUTINE %d\n", coro_data->id);
        clock_gettime(CLOCK_MONOTONIC, &coro_data->timer);
        ART_CO_YIELD(2);
    }
    ART_CO_STAGE(2) {
#define TIME_SECOND 1000000000
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t const start =
            coro_data->timer.tv_sec * TIME_SECOND + coro_data->timer.tv_nsec;
        uint64_t const end = now.tv_sec * TIME_SECOND + now.tv_nsec;
        uint64_t const diff = end - start;

        if (diff < (uint64_t)(lrand48() % 8) * TIME_SECOND) {
            ART_CO_YIELD(2);
        }
        ART_CO_YIELD(3);
    }
    ART_CO_STAGE(3) {
        LOG_INFO("FINISHED COROUTINE %d\n", coro_data->id);
        ART_CO_RETURN(4);
    }
    ART_CO_END();
}

int main() {
    int status = 0;
    ARTContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ARTContextSettings ctx_settings;
    memset(&ctx_settings, 0, sizeof(ctx_settings));
    ctx_settings.sched_count = 4;
    ctx_settings.use_current_thread = true;
    art_context_init(&ctx, &ctx_settings);

    srand48(time(NULL));
    ARTCoro *coros[] = {
        art_context_new_coro(&ctx),
        art_context_new_coro(&ctx),
        art_context_new_coro(&ctx),
        art_context_new_coro(&ctx),
    };
    for (int i = 0; i < 4; i++) {
        art_coro_init(coros[i], do_thing, &i, 1 << ART_CO_FREE);
    }
    LOG_INFO_("Pushing coroutines to global queue\n\n\n");
    sleep(2);
    art_context_run_coros(&ctx, coros, 4);
    art_context_join(&ctx);
    return status;
}
