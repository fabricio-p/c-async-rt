#ifndef ART_URL_H
#define ART_URL_H
#include <inttypes.h>

typedef struct {
    char const *begin;
    char const *end;
} CharRange;

typedef struct {
    CharRange
        scheme
    ,   host
    ,   path
    ,   query
    ,   fragment
    ;
    uint16_t port;
} ARTUrl;

ARTUrl
art_url_parse(CharRange raw);

#endif /* ART_URL_H */
