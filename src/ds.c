#include "ds.h"

static int
art_ds_darray_grow(
    void **data,
    size_t *capacity,
    size_t element_size
) {
    size_t new_cap = *capacity == 0 ? 8 : *capacity * 2;
    void *new_data = realloc(data, new_cap * element_size);
    if (new_data == NULL) {
        return 1;
    }

    *data = new_data;
    *capacity = new_cap;
    return 0;
}

int
art_ds_array_init(
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

uint32_t
art_ds_atomic_ref(atomic_uint_fast32_t *refcount) {
    return atomic_fetch_add_explicit(refcount, 1, memory_order_relaxed);
}

uint32_t
art_ds_atomic_unref(atomic_uint_fast32_t *refcount) {
    return atomic_fetch_sub_explicit(refcount, 1, memory_order_relaxed);
}
