#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Look up toneless pinyin `py`. Fills up to `max` candidate UTF-8 strings
 * (pointers valid until the next call). Returns the count. Everyday + Muse
 * gadget vocabulary; not a full IME lexicon. */
int muse_pinyin_lookup(const char *py, const char **out, int max);
#ifdef __cplusplus
}
#endif
