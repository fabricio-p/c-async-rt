#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "url.h"

ARTUrl
art_url_parse(CharRange raw) {
    ARTUrl url = {0};
    bool port_set = false;

    // NOTE: Doesn't do any checks
    char const *p = raw.begin;
    url.scheme.begin = p;
    for (; p < raw.end && *p != ':'; ++p);
    url.scheme.end = p;

    p += 3;
    url.host.begin = p;
    for (; p < raw.end && *p != ':' && *p != '/'; ++p);
    url.host.end = p;
    if (*p == ':') {
        CharRange port;
        port.begin = ++p;
        for (; p < raw.end && *p != '/'; ++p);
        port.end = p;
        if (port.end - port.begin > 5) {
            goto path_parsing;
        }
        char *end;
        uintmax_t parsed = strtoumax(port.begin, &end, 10);
        if (parsed > UINT16_MAX) {
            goto path_parsing;
        }
        url.port = parsed;
        port_set = true;
    }

    // NOTE: The ones that goto here are probably error cases
path_parsing:
    url.path.begin = ++p;
    for (; p < raw.end && *p != '?'; ++p);
    url.path.end = p;
    if (p == raw.end) {
        goto return_url;
    }

    url.query.begin = ++p;
    for (; p < raw.end && *p != '#'; ++p);
    url.query.end = p;
    if (p == raw.end) {
        goto return_url;
    }

    url.fragment.begin = ++p;
    for (; p < raw.end; ++p);
    url.fragment.end = p;
    if (p == raw.end) {
        goto return_url;
    }

return_url:
    if (!port_set) {
        size_t scheme_len = url.scheme.end - url.scheme.begin;
        if (strncmp(url.scheme.begin, "http", scheme_len) == 0) {
            url.port = 80;
        } else if (strncmp(url.scheme.begin, "https", scheme_len) == 0) {
            url.port = 443;
        }
    }
    return url;
}
