
#include "muse_pinyin.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_fail;

static int has_text(const muse_py_hit_t *h, int n, const char *t)
{
    for (int i = 0; i < n; i++) if (strcmp(h[i].text, t) == 0) return 1;
    return 0;
}

static void expect_has(const char *tag, const muse_py_hit_t *h, int n, const char *t)
{
    if (!has_text(h, n, t)) {
        fprintf(stderr, "FAIL %s: missing candidate '%s' (n=%d):", tag, t, n);
        for (int i = 0; i < n; i++) fprintf(stderr, " [%s/%u]", h[i].text, h[i].consume);
        fprintf(stderr, "\n");
        g_fail++;
    } else {
        printf("OK   %s has '%s'\n", tag, t);
    }
}

static void expect_n_gt(const char *tag, int n, int min)
{
    if (n < min) {
        fprintf(stderr, "FAIL %s: n=%d < %d\n", tag, n, min);
        g_fail++;
    } else {
        printf("OK   %s n=%d\n", tag, n);
    }
}

int main(void)
{
    muse_py_hit_t hits[16];
    int n;

    /* 26-key letter accumulation */
    n = muse_pinyin_lookup("t", hits, 16);
    expect_n_gt("letter t", n, 1);
    /* should NOT be stuck on only taiwan — include ta-prefix words AND syllables */
    n = muse_pinyin_lookup("tai", hits, 16);
    expect_has("tai", hits, n, "台湾");
    expect_has("tai", hits, n, "太");

    n = muse_pinyin_lookup("taiwan", hits, 16);
    expect_has("taiwan", hits, n, "台湾");

    n = muse_pinyin_lookup("nihao", hits, 16);
    expect_has("nihao", hits, n, "你好");

    n = muse_pinyin_lookup("ninhao", hits, 16);
    expect_has("ninhao", hits, n, "您好");

    /* multi-key sequence: ni then hao — partial word */
    n = muse_pinyin_lookup("ni", hits, 16);
    expect_has("ni", hits, n, "你");
    expect_has("ni-word", hits, n, "你好");

    n = muse_pinyin_lookup("nih", hits, 16);
    expect_has("nih", hits, n, "你好");

    /* after first syllable when typing nihao, 你 available with consume=2 */
    n = muse_pinyin_lookup("nihao", hits, 16);
    {
        int found = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(hits[i].text, "你") == 0 && hits[i].consume == 2) found = 1;
        }
        if (!found) {
            /* 你好 exact may dominate; still require 你 with consume 2 from step 5 */
            fprintf(stderr, "FAIL nihao: missing 你 consume=2\n");
            for (int i = 0; i < n; i++)
                fprintf(stderr, "  [%s/%u]\n", hits[i].text, hits[i].consume);
            g_fail++;
        } else {
            printf("OK   nihao has 你 consume=2\n");
        }
    }

    /* T9: taiwan = 8-2-4-9-2-6 */
    n = muse_pinyin_lookup_t9("8", hits, 16);
    expect_n_gt("t9 8", n, 1);
    n = muse_pinyin_lookup_t9("82", hits, 16);
    expect_n_gt("t9 82", n, 1);
    /* 82 should keep candidates (ta...), NOT clear */
    expect_has("t9 82 taiwan-prefix", hits, n, "台湾");

    n = muse_pinyin_lookup_t9("824926", hits, 16);
    expect_has("t9 taiwan", hits, n, "台湾");

    n = muse_pinyin_lookup_t9("64426", hits, 16);
    expect_has("t9 nihao", hits, n, "你好");

    n = muse_pinyin_lookup_t9("646426", hits, 16);
    expect_has("t9 ninhao", hits, n, "您好");

    /* T9 multi-key: 8 then 5 (TUV+JKL) — may have few/no words, but must refresh
     * without crashing; prefix-of-syllable path still OK to return 0. */
    n = muse_pinyin_lookup_t9("85", hits, 16);
    printf("OK   t9 85 n=%d (may be 0; must not stick on 台湾)\n", n);
    if (has_text(hits, n, "台湾")) {
        fprintf(stderr, "FAIL t9 85 should not still list 台湾\n");
        g_fail++;
    }

    /* letter digit helper */
    if (muse_pinyin_letter_digit('t') != '8' || muse_pinyin_letter_digit('a') != '2') {
        fprintf(stderr, "FAIL letter_digit\n");
        g_fail++;
    } else {
        printf("OK   letter_digit\n");
    }

    if (g_fail) {
        fprintf(stderr, "\n%d failure(s)\n", g_fail);
        return 1;
    }
    printf("\nAll pinyin IME tests passed.\n");
    return 0;
}
