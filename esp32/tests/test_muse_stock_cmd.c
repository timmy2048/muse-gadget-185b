/* Host test for muse_stock_cmd.h. Build: gcc -o /tmp/tsc test_muse_stock_cmd.c && /tmp/tsc */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../components/muse/muse_stock_cmd.h"

static int s_applied;
static int on_cmd(const muse_stock_cmd_t *c, void *u)
{
    (void)u;
    assert(c->op == MUSE_STOCK_OP_ADD);
    assert(!strcmp(c->symbol, "AAPL"));
    s_applied++;
    return 0;
}

int main(void)
{
    muse_stock_cmd_t cmd;
    assert(muse_stock_parse_cmd("{\"op\":\"add\",\"symbol\":\"aapl\"}", &cmd) == 0);
    assert(cmd.op == MUSE_STOCK_OP_ADD && !strcmp(cmd.symbol, "AAPL"));
    assert(muse_stock_parse_cmd("{\"op\":\"remove\",\"sym\":\"SPCX\"}", &cmd) == 0);
    assert(cmd.op == MUSE_STOCK_OP_REMOVE && !strcmp(cmd.symbol, "SPCX"));
    assert(muse_stock_parse_cmd("{\"op\":\"clear\"}", &cmd) == 0);
    assert(cmd.op == MUSE_STOCK_OP_CLEAR);
    assert(muse_stock_parse_cmd("{\"op\":\"replace\",\"symbols\":[\"META\",\"lite\"]}", &cmd) == 0);
    assert(cmd.op == MUSE_STOCK_OP_REPLACE && cmd.nsymbols == 2);
    assert(!strcmp(cmd.symbols[0], "META") && !strcmp(cmd.symbols[1], "LITE"));

    char text[256];
    snprintf(text, sizeof(text),
             "ok <<<MUSE_STOCK{\"op\":\"add\",\"symbol\":\"AAPL\"}>>> bye");
    s_applied = 0;
    int got = muse_stock_scan_markers(text, on_cmd, NULL);
    assert(got == 1 && s_applied == 1);
    assert(!strstr(text, "MUSE_STOCK"));
    assert(strstr(text, "ok"));
    assert(strstr(text, "bye"));
    puts("muse_stock_cmd unit asserts OK");
    return 0;
}
