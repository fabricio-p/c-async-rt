- [x] [`src/chan.c`:`26`]
    Find a way to do the direct transfer optimization.
    Also the receivers thing should be a list of other things that specify the coroutine and the address where the value should be stored.
    The coroutine should be put in the `recv_l` list of the scheduler.
- [ ] Maybe after the coroutine has received the value we should push it to
    the buffer queue of its scheduler? Need to think more about this.
    If yes, need to make sure we're locking `buffer_q` when pushing.

- [x] [`src/queues.h`:`16`]
    Need to make a generic queue/deque implementation thing in `src/ds.{c,h}`. Optional locking features perhaps.

- [ ] [`src/coro.h`:`33`]
    Allocation strategy for `ARTChanCoro`. `malloc()` and `free()` would be very wasteful, we need a pool of these. When an entry is not being used the `prev`,`next` point to the previous and next elements of the free list.
