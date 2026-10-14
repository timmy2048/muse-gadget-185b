/*
 * Pure-C parsers for Muse todo list / chat commands (host-testable).
 *
 * List JSON (todos.json / NVS):
 *   [{"id":"a1","title":"买牛奶","due":1728900000,"done":false}, ...]
 *
 * Chat / device.todo command markers:
 *   <<<MUSE_TODO{"op":"add","title":"买牛奶","due":1728900000}>>>
 *   <<<MUSE_TODO{"op":"complete","id":"a1"}>>>
 *   <<<MUSE_TODO{"op":"remove","id":"a1"}>>>
 *   <<<MUSE_TODO{"op":"clear"}>>>
 *   <<<MUSE_TODO{"op":"replace","items":[...]}>>>
 *
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define MUSE_TODO_ID_LEN 16
#define MUSE_TODO_TITLE_LEN 64
#define MUSE_TODO_MAX 12
#define MUSE_TODO_MARKER_OPEN "<<<MUSE_TODO"
#define MUSE_TODO_MARKER_CLOSE ">>>"

typedef struct {
    char id[MUSE_TODO_ID_LEN];
    char title[MUSE_TODO_TITLE_LEN];
    int64_t due; /* unix seconds; 0 = no due */
    bool done;
    bool reminded; /* already fired due reminder this due window */
} muse_todo_item_t;

typedef enum {
    MUSE_TODO_OP_NONE = 0,
    MUSE_TODO_OP_ADD,
    MUSE_TODO_OP_COMPLETE,
    MUSE_TODO_OP_REMOVE,
    MUSE_TODO_OP_CLEAR,
    MUSE_TODO_OP_REPLACE,
} muse_todo_op_t;

typedef struct {
    muse_todo_op_t op;
    char id[MUSE_TODO_ID_LEN];
    char title[MUSE_TODO_TITLE_LEN];
    int64_t due;
    muse_todo_item_t items[MUSE_TODO_MAX];
    int nitems;
} muse_todo_cmd_t;

/* Skip whitespace. */
static inline const char *todo_skip_ws(const char *p)
{
    while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        p++;
    }
    return p;
}

/* Extract JSON string value after "key": into out (cap bytes). Returns 0 on ok. */
static inline int todo_json_str(const char *json, const char *key, char *out, size_t cap)
{
    if (!json || !key || !out || cap == 0) {
        return -1;
    }
    const char *p = strstr(json, key);
    if (!p) {
        return -1;
    }
    p += strlen(key);
    p = todo_skip_ws(p);
    if (*p == ':') {
        p++;
    }
    p = todo_skip_ws(p);
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

/* Number after "key": ; returns 0 on ok. */
static inline int todo_json_i64(const char *json, const char *key, int64_t *out)
{
    if (!json || !key || !out) {
        return -1;
    }
    const char *p = strstr(json, key);
    if (!p) {
        return -1;
    }
    p += strlen(key);
    p = todo_skip_ws(p);
    if (*p == ':') {
        p++;
    }
    p = todo_skip_ws(p);
    char *end = NULL;
    long long v = strtoll(p, &end, 10);
    if (end == p) {
        return -1;
    }
    *out = (int64_t)v;
    return 0;
}

static inline int todo_json_bool(const char *json, const char *key, bool *out)
{
    if (!json || !key || !out) {
        return -1;
    }
    const char *p = strstr(json, key);
    if (!p) {
        return -1;
    }
    p += strlen(key);
    p = todo_skip_ws(p);
    if (*p == ':') {
        p++;
    }
    p = todo_skip_ws(p);
    if (!strncmp(p, "true", 4)) {
        *out = true;
        return 0;
    }
    if (!strncmp(p, "false", 5)) {
        *out = false;
        return 0;
    }
    return -1;
}

/* Parse one object {...} into item. Returns bytes consumed (>0) or -1. */
static inline int todo_parse_item_obj(const char *json, muse_todo_item_t *it)
{
    if (!json || !it || *json != '{') {
        return -1;
    }
    int depth = 0;
    const char *p = json;
    do {
        if (*p == '{') {
            depth++;
        } else if (*p == '}') {
            depth--;
        }
        p++;
    } while (*p && depth > 0);
    if (depth != 0) {
        return -1;
    }
    size_t len = (size_t)(p - json);
    char buf[512];
    if (len >= sizeof(buf)) {
        return -1;
    }
    memcpy(buf, json, len);
    buf[len] = '\0';

    memset(it, 0, sizeof(*it));
    if (todo_json_str(buf, "\"id\"", it->id, sizeof(it->id)) != 0) {
        it->id[0] = '\0';
    }
    if (todo_json_str(buf, "\"title\"", it->title, sizeof(it->title)) != 0 || !it->title[0]) {
        return -1;
    }
    int64_t due = 0;
    if (todo_json_i64(buf, "\"due\"", &due) == 0) {
        it->due = due;
    }
    bool done = false;
    if (todo_json_bool(buf, "\"done\"", &done) == 0) {
        it->done = done;
    }
    bool rem = false;
    if (todo_json_bool(buf, "\"reminded\"", &rem) == 0) {
        it->reminded = rem;
    }
    return (int)len;
}

/* Parse JSON array of todo objects. Returns count, or -1. */
static inline int muse_todo_parse_list(const char *json, muse_todo_item_t *out, int max)
{
    if (!json || !out || max <= 0) {
        return -1;
    }
    const char *p = todo_skip_ws(json);
    if (*p != '[') {
        return -1;
    }
    p++;
    int n = 0;
    while (*p && n < max) {
        p = todo_skip_ws(p);
        if (*p == ']') {
            return n;
        }
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '{') {
            return -1;
        }
        int used = todo_parse_item_obj(p, &out[n]);
        if (used < 0) {
            return -1;
        }
        p += used;
        n++;
    }
    p = todo_skip_ws(p);
    return (*p == ']' || n == max) ? n : -1;
}

/* Serialize list to JSON (compact). Returns bytes written (excl NUL), or -1. */
static inline int muse_todo_list_to_json(const muse_todo_item_t *items, int n, char *out, size_t cap)
{
    if (!out || cap < 3) {
        return -1;
    }
    size_t o = 0;
    out[o++] = '[';
    for (int i = 0; i < n; i++) {
        char one[192];
        int m = snprintf(one, sizeof(one),
                         "%s{\"id\":\"%s\",\"title\":\"%s\",\"due\":%lld,\"done\":%s,\"reminded\":%s}",
                         i ? "," : "",
                         items[i].id,
                         items[i].title,
                         (long long)items[i].due,
                         items[i].done ? "true" : "false",
                         items[i].reminded ? "true" : "false");
        if (m < 0 || o + (size_t)m + 2 >= cap) {
            return -1;
        }
        memcpy(out + o, one, (size_t)m);
        o += (size_t)m;
    }
    out[o++] = ']';
    out[o] = '\0';
    return (int)o;
}

/* Parse one command object. Returns 0 on success. */
static inline int muse_todo_parse_cmd(const char *json, muse_todo_cmd_t *cmd)
{
    if (!json || !cmd) {
        return -1;
    }
    memset(cmd, 0, sizeof(*cmd));
    char op[24];
    if (todo_json_str(json, "\"op\"", op, sizeof(op)) != 0) {
        return -1;
    }
    if (!strcmp(op, "add")) {
        cmd->op = MUSE_TODO_OP_ADD;
        if (todo_json_str(json, "\"title\"", cmd->title, sizeof(cmd->title)) != 0 || !cmd->title[0]) {
            return -1;
        }
        todo_json_str(json, "\"id\"", cmd->id, sizeof(cmd->id));
        int64_t due = 0;
        if (todo_json_i64(json, "\"due\"", &due) == 0) {
            cmd->due = due;
        }
        return 0;
    }
    if (!strcmp(op, "complete") || !strcmp(op, "done")) {
        cmd->op = MUSE_TODO_OP_COMPLETE;
        if (todo_json_str(json, "\"id\"", cmd->id, sizeof(cmd->id)) != 0) {
            /* allow match by title */
            todo_json_str(json, "\"title\"", cmd->title, sizeof(cmd->title));
        }
        if (!cmd->id[0] && !cmd->title[0]) {
            return -1;
        }
        return 0;
    }
    if (!strcmp(op, "remove") || !strcmp(op, "delete")) {
        cmd->op = MUSE_TODO_OP_REMOVE;
        todo_json_str(json, "\"id\"", cmd->id, sizeof(cmd->id));
        todo_json_str(json, "\"title\"", cmd->title, sizeof(cmd->title));
        if (!cmd->id[0] && !cmd->title[0]) {
            return -1;
        }
        return 0;
    }
    if (!strcmp(op, "clear")) {
        cmd->op = MUSE_TODO_OP_CLEAR;
        return 0;
    }
    if (!strcmp(op, "replace") || !strcmp(op, "sync")) {
        cmd->op = MUSE_TODO_OP_REPLACE;
        const char *items = strstr(json, "\"items\"");
        if (!items) {
            return -1;
        }
        items = strchr(items, '[');
        if (!items) {
            return -1;
        }
        cmd->nitems = muse_todo_parse_list(items, cmd->items, MUSE_TODO_MAX);
        return cmd->nitems >= 0 ? 0 : -1;
    }
    return -1;
}

/*
 * Find/apply all <<<MUSE_TODO...>>> markers in text.
 * Calls apply(cmd, user) for each valid command.
 * Strips markers from text in place (keeps surrounding prose).
 * Returns number of commands applied.
 */
static inline int muse_todo_scan_markers(char *text,
                                         int (*apply)(const muse_todo_cmd_t *cmd, void *user),
                                         void *user)
{
    if (!text || !apply) {
        return 0;
    }
    int applied = 0;
    char *p = text;
    while ((p = strstr(p, MUSE_TODO_MARKER_OPEN)) != NULL) {
        char *body = p + strlen(MUSE_TODO_MARKER_OPEN);
        char *end = strstr(body, MUSE_TODO_MARKER_CLOSE);
        if (!end) {
            break;
        }
        size_t blen = (size_t)(end - body);
        char buf[768];
        if (blen >= sizeof(buf)) {
            p = end + strlen(MUSE_TODO_MARKER_CLOSE);
            continue;
        }
        memcpy(buf, body, blen);
        buf[blen] = '\0';
        muse_todo_cmd_t cmd;
        if (muse_todo_parse_cmd(buf, &cmd) == 0) {
            if (apply(&cmd, user) == 0) {
                applied++;
            }
        }
        /* Remove marker from text (including optional surrounding whitespace). */
        char *cut_end = end + strlen(MUSE_TODO_MARKER_CLOSE);
        while (p > text && (p[-1] == ' ' || p[-1] == '\n')) {
            p--;
        }
        while (*cut_end == ' ' || *cut_end == '\n') {
            cut_end++;
        }
        memmove(p, cut_end, strlen(cut_end) + 1);
        /* continue from p (content after removal) */
    }
    /* Trim trailing whitespace left by stripping. */
    size_t n = strlen(text);
    while (n > 0 && (text[n - 1] == ' ' || text[n - 1] == '\n' || text[n - 1] == '\r')) {
        text[--n] = '\0';
    }
    return applied;
}
