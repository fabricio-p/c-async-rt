#ifndef ART_LOGGING_H
#define ART_LOGGING_H

#define LOG_INFO_(fmtstr)                                                   \
    fprintf(stderr, "[INFO:" __FILE__ ":%d] " fmtstr, __LINE__)

#define LOG_INFO(fmtstr, ...)                                               \
    fprintf(stderr, "[INFO:" __FILE__ ":%d] " fmtstr, __LINE__, __VA_ARGS__)

#endif /* ART_LOGGING_H */
