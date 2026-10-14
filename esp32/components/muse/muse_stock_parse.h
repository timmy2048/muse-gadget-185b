/*
 * Yahoo Finance chart JSON parser for muse_stock (pure C, host-testable).
 *
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once

#include <stdlib.h>
#include <string.h>

/* Number following "key": in json; returns 0 and sets *out on success. */
static inline int stock_json_num(const char *json, const char *key, double *out)
{
    const char *p = strstr(json, key);
    if (!p) {
        return -1;
    }
    p += strlen(key);
    while (*p == ' ' || *p == ':' || *p == '[') {
        p++;
    }
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p || v != v) {
        return -1;
    }
    *out = v;
    return 0;
}

/*
 * Parse /v8/finance/chart/<SYM>?interval=1d&range=1d.
 * price = meta.regularMarketPrice, prev = meta.chartPreviousClose,
 * open = indicators.quote[0].open[0] (falls back to prev if absent/null).
 * Returns 0 on success.
 */
static inline int stock_parse_chart(const char *json, double *price, double *prev, double *open)
{
    double p, c, o;
    if (!json || strstr(json, "\"error\":{")) {
        return -1;
    }
    if (stock_json_num(json, "\"regularMarketPrice\"", &p) != 0 ||
        stock_json_num(json, "\"chartPreviousClose\"", &c) != 0 || p <= 0 || c <= 0) {
        return -1;
    }
    if (stock_json_num(json, "\"open\"", &o) != 0 || o <= 0) {
        o = c;
    }
    *price = p;
    *prev = c;
    *open = o;
    return 0;
}
