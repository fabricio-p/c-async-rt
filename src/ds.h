#ifndef ASYNCRT_DS_H
#define ASYNCRT_DS_H
#include <stdlib.h>
#include <inttypes.h>
#include <stdatomic.h>

#define LIST_PARTS(PREFIX, TYPE) TYPE *PREFIX##next
#define DOUBLE_LIST_PARTS(PREFIX, TYPE) \
    LIST_PARTS(PREFIX, TYPE);           \
    TYPE *PREFIX##prev

#define ARRAY_PARTS(PREFIX, TYPE)       \
    TYPE *PREFIX##items;                \
    size_t PREFIX##size

#define DARRAY_PARTS(PREFIX, TYPE)      \
    ARRAY_PARTS(PREFIX, TYPE);          \
    size_t PREFIX##capacity

#define ATOMIC_RC_PARTS(PREFIX) \
    atomic_uint_fast32_t  PREFIX##refcount

#define ATOMIC_RC_WEAK_PARTS(PREFIX) \
    atomic_uint_fast32_t PREFIX##weak_count

// typedef struct ARTRefCounts {
//     uint32_t strong;
//     uint32_t weak;
// } ARTRefCounts;

#define LIST_NEXT(X, PREFIX) (X)->PREFIX##next
#define DOUBLE_LIST_NEXT(X, PREFIX) LIST_NEXT(X, PREFIX)
#define DOUBLE_LIST_PREV(X, PREFIX) (X)->PREFIX##prev
#define ATOMIC_REF(X, PREFIX) art_ds_atomic_ref(&(X)->PREFIX##refcount)
#define ATOMIC_UNREF(X, PREFIX) art_ds_atomic_unref(&(X)->PREFIX##refcount)
// #define ATOMIC_WEAK_REF(X, PREFIX) art_ds_atomic_weak_ref(&(X)->PREFIX##refcount, &(X)->PREFIX##weak_count)
// #define ATOMIC_WEAK_UNREF(X, PREFIX) art_ds_atomic_unref(&(X)->PREFIX##weak_count)

#define DARRAY_INIT(X, PREFIX, TYPE, CAP)   \
    art_ds_darray_init(                     \
        &(X)->PREFIX##items,                \
        &(X)->PREFIX##size,                 \
        &(X)->PREFIX##capacity,             \
        sizeof(TYPE),                       \
        CAP                                 \
    )

#define DARRAY_UNINIT(X, PREFIX)            \
    art_ds_darray_uninit(                   \
        (void **)&(X)->PREFIX##items,       \
        &(X)->PREFIX##size,                 \
        &(X)->PREFIX##capacity              \
    )

#define DARRAY_PUSH(X, PREFIX, TYPE)    \
    ((TYPE *) art_ds_darray_push(       \
        (void **)&(X)->PREFIX##items,   \
        &(X)->PREFIX##size,             \
        &(X)->PREFIX##capacity,         \
        sizeof(TYPE)                    \
    ))

int
art_ds_darray_init(
    void **items_p,
    size_t *size_p,
    size_t *cap_p,
    size_t element_size,
    size_t cap
);

static inline void
art_ds_darray_uninit(
    void **items_p,
    size_t *size_p,
    size_t *cap_p
) {
    free(*items_p);
    *items_p = NULL;
    *size_p = 0;
    *cap_p = 0;
}

void *
art_ds_darray_push(
    void **items,
    size_t *size,
    size_t *capacity,
    size_t element_size
);

uint32_t
art_ds_atomic_ref(atomic_uint_fast32_t *refcount);

uint32_t
art_ds_atomic_unref(atomic_uint_fast32_t *refcount);

#endif /* ASYNCRT_DS_H */
