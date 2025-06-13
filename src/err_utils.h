#ifndef ART_ERR_UTILS_H
#define ART_ERR_UTILS_H

#define THROW(status_, label)                                               \
    do {                                                                    \
        status = (status);                                                  \
        goto catch_##label;                                                 \
    } while (0)
#define THROW_IF(cond, status_, label)                                      \
    if (cond) {                                                             \
        THROW(status_, label);                                              \
    }

#define RETURN(label) goto catch_##label

#endif /* ART_ERR_UTILS_H */
