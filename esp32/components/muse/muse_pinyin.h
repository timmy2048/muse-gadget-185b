#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* One IME candidate: UTF-8 text + how many input units to consume
 * (letters for 26-key / abc pinyin, digits for T9). */
typedef struct {
    const char *text;
    uint8_t consume;
} muse_py_hit_t;

/* Toneless pinyin letter buffer (a-z), e.g. "nihao" / "taiwan". */
int muse_pinyin_lookup(const char *py, muse_py_hit_t *out, int max);

/* T9 digit buffer (2-9), e.g. "644266" for nihao, "824926" for taiwan. */
int muse_pinyin_lookup_t9(const char *digits, muse_py_hit_t *out, int max);

/* Map a-z to T9 digit '2'..'9'; 0 if not a letter. */
char muse_pinyin_letter_digit(char c);

#ifdef __cplusplus
}
#endif
