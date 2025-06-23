#include "ds.h"

static int
art_ds_darray_grow(
    void **data,
    size_t *capacity,
    size_t element_size
) {
    size_t new_cap = *capacity == 0 ? 8 : *capacity * 2;
    void *new_data = realloc(*data, new_cap * element_size);
    if (new_data == NULL) {
        return 1;
    }

    *data = new_data;
    *capacity = new_cap;
    return 0;
}

int
art_ds_darray_init(
    void **data_p,
    size_t *size_p,
    size_t *cap_p,
    size_t element_size,
    size_t cap
) {
    *data_p = malloc(cap * element_size);
    if (*data_p == NULL) {
        return 1;
    }

    *size_p = 0;
    *cap_p = cap;
    return 0;
}

void *
art_ds_darray_push(
    void **data,
    size_t *size,
    size_t *capacity,
    size_t element_size
) {
    if (*size == *capacity) {
        if (art_ds_darray_grow(data, capacity, element_size) != 0) {
            return NULL;
        }
    }

    return data[(*size)++ * element_size];
}

void
art_ds_deque_push_back(
    void **first_p,
    void **last_p,
    void *item,
    ptrdiff_t prev_offset,
    ptrdiff_t next_offset
) {
    if (*first_p == NULL) {
        *first_p = item;
        *last_p = item;
    } else {
        void **prev_p = (void **)((uint8_t *)item + prev_offset);
        void **last_next_p = (void **)((uint8_t *)*last_p + next_offset);
        *last_next_p = item;
        *prev_p = *last_p;
        *last_p = item;
    }
}

void *
art_ds_deque_pop_front(
    void **first_p,
    void **last_p,
    ptrdiff_t prev_offset,
    ptrdiff_t next_offset
) {
    void *item = *first_p;
    if (item == NULL) {
        return NULL;
    }
    void **next_p = (void **)((uint8_t *)item + next_offset);
    *first_p = *next_p;
    if (*last_p == item) {
        *last_p = NULL;
    } else {
        void **prev_p = (void **)((uint8_t *)*next_p + prev_offset);
        *prev_p = NULL;
    }
    *next_p = NULL;
    return item;
}

uint32_t
art_ds_atomic_ref(atomic_uint_fast32_t *refcount) {
    return atomic_fetch_add_explicit(refcount, 1, memory_order_relaxed);
}

uint32_t
art_ds_atomic_unref(atomic_uint_fast32_t *refcount) {
    return atomic_fetch_sub_explicit(refcount, 1, memory_order_relaxed);
}
