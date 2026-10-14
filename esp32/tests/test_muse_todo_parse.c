/* Host test for muse_todo_parse.h. Build: gcc -o /tmp/t test_muse_todo_parse.c && /tmp/t */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../components/muse/muse_todo_parse.h"

static int s_applied;
static int on_cmd(const muse_todo_cmd_t *c, void *u)
{
    (void)u;
    assert(c->op == MUSE_TODO_OP_ADD);
    assert(!strcmp(c->title, "买牛奶"));
    s_applied++;
    return 0;
}

int main(void)
{
    muse_todo_item_t items[MUSE_TODO_MAX];
    const char *list = "[{\"id\":\"a1\",\"title\":\"milk\",\"due\":1728900000,\"done\":false},"
                       "{\"id\":\"a2\",\"title\":\"meet\",\"due\":0,\"done\":true}]";
    int n = muse_todo_parse_list(list, items, MUSE_TODO_MAX);
    assert(n == 2);
    assert(!strcmp(items[0].id, "a1"));
    assert(!strcmp(items[0].title, "milk"));
    assert(items[0].due == 1728900000);
    assert(!items[0].done);
    assert(items[1].done);

    char json[1024];
    assert(muse_todo_list_to_json(items, n, json, sizeof(json)) > 0);
    muse_todo_item_t round[MUSE_TODO_MAX];
    assert(muse_todo_parse_list(json, round, MUSE_TODO_MAX) == 2);

    muse_todo_cmd_t cmd;
    assert(muse_todo_parse_cmd("{\"op\":\"add\",\"title\":\"mail\",\"due\":99}", &cmd) == 0);
    assert(cmd.op == MUSE_TODO_OP_ADD && cmd.due == 99);
    assert(muse_todo_parse_cmd("{\"op\":\"complete\",\"id\":\"a1\"}", &cmd) == 0);
    assert(cmd.op == MUSE_TODO_OP_COMPLETE);
    assert(muse_todo_parse_cmd("{\"op\":\"remove\",\"title\":\"meet\"}", &cmd) == 0);
    assert(cmd.op == MUSE_TODO_OP_REMOVE);
    assert(muse_todo_parse_cmd("{\"op\":\"clear\"}", &cmd) == 0);
    assert(cmd.op == MUSE_TODO_OP_CLEAR);
    assert(muse_todo_parse_cmd("{\"op\":\"replace\",\"items\":[{\"title\":\"x\",\"due\":1}]}", &cmd) == 0);
    assert(cmd.op == MUSE_TODO_OP_REPLACE && cmd.nitems == 1);

    char text[256];
    snprintf(text, sizeof(text),
             "ok <<<MUSE_TODO{\"op\":\"add\",\"title\":\"买牛奶\",\"due\":1}>>> bye");
    s_applied = 0;
    int got = muse_todo_scan_markers(text, on_cmd, NULL);
    assert(got == 1 && s_applied == 1);
    assert(!strstr(text, "MUSE_TODO"));
    assert(strstr(text, "ok"));
    assert(strstr(text, "bye"));

    puts("muse_todo_parse unit asserts OK");
    return 0;
}
