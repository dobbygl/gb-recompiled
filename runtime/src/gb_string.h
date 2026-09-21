/* Private C-compatible host string operations, with no global tokenizer state. */
#ifndef GBRT_STRING_H
#define GBRT_STRING_H
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif

static inline int gb_strcasecmp(const char *left, const char *right) {
#ifdef _WIN32
    return _stricmp(left, right);
#else
    return strcasecmp(left, right);
#endif
}

static inline char *gb_strtok_r(char *text, const char *delimiters, char **context) {
    /* A completed iteration stays completed, including with the Windows CRT's
     * invalid-parameter checks when its saved context is NULL. */
    if (!text && !*context)
        return NULL;
#ifdef _WIN32
    return strtok_s(text, delimiters, context);
#else
    return strtok_r(text, delimiters, context);
#endif
}
#endif
