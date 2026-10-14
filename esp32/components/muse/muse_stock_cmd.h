/*
 * Chat / device.stock command markers for muse_stock watchlist (pure C, host-testable).
 *
 * Chat markers (same pattern as todos):
 *   <<<MUSE_STOCK{"op":"add","symbol":"AAPL"}>>>
 *   <<<MUSE_STOCK{"op":"remove","symbol":"SPCX"}>>>
 *   <<<MUSE_STOCK{"op":"clear"}>>>
 *   <<<MUSE_STOCK{"op":"replace","symbols":["META","AAPL"]}>>>
 *
 * Also accepts "sym" as an alias for "symbol".
 *
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MUSE_STOCK_SYM_LEN 16
#define MUSE_STOCK_CMD_MAX 8
#define MUSE_STOCK_MARKER_OPEN "<<<MUSE_STOCK"
#define MUSE_STOCK_MARKER_CLOSE ">>>"

typedef enum {
    MUSE_STOCK_OP_NONE = 0,
    MUSE_STOCK_OP_ADD,
    MUSE_STOCK_OP_REMOVE,
    MUSE_STOCK_OP_CLEAR,
    MUSE_STOCK_OP_REPLACE,
} muse_stock_op_t;

typedef struct {
    muse_stock_op_t op;
    char symbol[MUSE_STOCK_SYM_LEN];
    char symbols[MUSE_STOCK_CMD_MAX][MUSE_STOCK_SYM_LEN];
    int nsymbols;
} muse_stock_cmd_t;

static inline const char *stock_cmd_skip_ws(const char *p)
{
    while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        p++;
    }
    return p;
}

static inline int stock_cmd_json_str(const char *json, const char *key, char *out, size_t cap)
{
    if (!json || !key || !out || cap == 0) {
        return -1;
    }
    const char *p = strstr(json, key);
    if (!p) {
        return -1;
    }
    p += strlen(key);
    p = stock_cmd_skip_ws(p);
    if (*p == ':') {
        p++;
    }
    p = stock_cmd_skip_ws(p);
    if (*p != '"') {
        return -1;
    }
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
        }
        out[n++] = *p++;
    }
    out[n] = '\0';
    return (*p == '"') ? 0 : -1;
}

/* Uppercase ticker into dst; returns 0 if non-empty and fits. */
static inline int stock_cmd_norm_sym(const char *src, char *dst, size_t cap)
{
    if (!src || !dst || cap < 2) {
        return -1;
    }
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < cap; i++) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-') {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
    return j > 0 ? 0 : -1;
}

/* Parse JSON array of symbol strings into out[][MUSE_STOCK_SYM_LEN]. */
static inline int muse_stock_parse_symbols(const char *json, char out[][MUSE_STOCK_SYM_LEN], int max)
{
    if (!json || !out || max <= 0) {
        return -1;
    }
    const char *p = strchr(json, '[');
    if (!p) {
        return -1;
    }
    p++;
    int n = 0;
    while (*p && n < max) {
        p = stock_cmd_skip_ws(p);
        if (*p == ']') {
            break;
        }
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '"') {
            break;
        }
        p++;
        char raw[MUSE_STOCK_SYM_LEN];
        size_t k = 0;
        while (*p && *p != '"' && k + 1 < sizeof(raw)) {
            raw[k++] = *p++;
        }
        raw[k] = '\0';
        if (*p == '"') {
            p++;
        }
        if (stock_cmd_norm_sym(raw, out[n], MUSE_STOCK_SYM_LEN) == 0) {
            n++;
        }
    }
    return n;
}

static inline int muse_stock_parse_cmd(const char *json, muse_stock_cmd_t *cmd)
{
    if (!json || !cmd) {
        return -1;
    }
    memset(cmd, 0, sizeof(*cmd));
    char op[24];
    if (stock_cmd_json_str(json, "\"op\"", op, sizeof(op)) != 0) {
        return -1;
    }
    if (!strcmp(op, "add")) {
        cmd->op = MUSE_STOCK_OP_ADD;
        if (stock_cmd_json_str(json, "\"symbol\"", cmd->symbol, sizeof(cmd->symbol)) != 0 &&
            stock_cmd_json_str(json, "\"sym\"", cmd->symbol, sizeof(cmd->symbol)) != 0) {
            return -1;
        }
        char norm[MUSE_STOCK_SYM_LEN];
        if (stock_cmd_norm_sym(cmd->symbol, norm, sizeof(norm)) != 0) {
            return -1;
        }
        snprintf(cmd->symbol, sizeof(cmd->symbol), "%s", norm);
        return 0;
    }
    if (!strcmp(op, "remove") || !strcmp(op, "delete") || !strcmp(op, "del")) {
        cmd->op = MUSE_STOCK_OP_REMOVE;
        if (stock_cmd_json_str(json, "\"symbol\"", cmd->symbol, sizeof(cmd->symbol)) != 0 &&
            stock_cmd_json_str(json, "\"sym\"", cmd->symbol, sizeof(cmd->symbol)) != 0) {
            return -1;
        }
        char norm[MUSE_STOCK_SYM_LEN];
        if (stock_cmd_norm_sym(cmd->symbol, norm, sizeof(norm)) != 0) {
            return -1;
        }
        snprintf(cmd->symbol, sizeof(cmd->symbol), "%s", norm);
        return 0;
    }
    if (!strcmp(op, "clear")) {
        cmd->op = MUSE_STOCK_OP_CLEAR;
        return 0;
    }
    if (!strcmp(op, "replace")) {
        cmd->op = MUSE_STOCK_OP_REPLACE;
        const char *items = strstr(json, "\"symbols\"");
        if (!items) {
            return -1;
        }
        items = strchr(items, '[');
        if (!items) {
            return -1;
        }
        cmd->nsymbols = muse_stock_parse_symbols(items, cmd->symbols, MUSE_STOCK_CMD_MAX);
        return cmd->nsymbols >= 0 ? 0 : -1;
    }
    return -1;
}

/*
 * Find/apply all <<<MUSE_STOCK...>>> markers in text.
 * Strips markers in place. Returns number of commands applied.
 */
static inline int muse_stock_scan_markers(char *text,
                                          int (*apply)(const muse_stock_cmd_t *cmd, void *user),
                                          void *user)
{
    if (!text || !apply) {
        return 0;
    }
    int applied = 0;
    char *p = text;
    while ((p = strstr(p, MUSE_STOCK_MARKER_OPEN)) != NULL) {
        char *body = p + strlen(MUSE_STOCK_MARKER_OPEN);
        char *end = strstr(body, MUSE_STOCK_MARKER_CLOSE);
        if (!end) {
            break;
        }
        size_t blen = (size_t)(end - body);
        char buf[512];
        if (blen >= sizeof(buf)) {
            p = end + strlen(MUSE_STOCK_MARKER_CLOSE);
            continue;
        }
        memcpy(buf, body, blen);
        buf[blen] = '\0';
        muse_stock_cmd_t cmd;
        if (muse_stock_parse_cmd(buf, &cmd) == 0) {
            if (apply(&cmd, user) == 0) {
                applied++;
            }
        }
        char *cut_end = end + strlen(MUSE_STOCK_MARKER_CLOSE);
        while (p > text && (p[-1] == ' ' || p[-1] == '\n')) {
            p--;
        }
        while (*cut_end == ' ' || *cut_end == '\n') {
            cut_end++;
        }
        memmove(p, cut_end, strlen(cut_end) + 1);
    }
    size_t n = strlen(text);
    while (n > 0 && (text[n - 1] == ' ' || text[n - 1] == '\n' || text[n - 1] == '\r')) {
        text[--n] = '\0';
    }
    return applied;
}
