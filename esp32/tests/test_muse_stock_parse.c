/* Host test for muse_stock_parse.h. Usage: test_muse_stock_parse file.json... */
#include <stdio.h>
#include <assert.h>
#include "../components/muse/muse_stock_parse.h"
int main(int argc, char **argv)
{
    double p, c, o;
    assert(stock_parse_chart("{\"meta\":{\"regularMarketPrice\":10.5,\"chartPreviousClose\":10},"
                             "\"indicators\":{\"quote\":[{\"open\":[9.75]}]}}", &p, &c, &o) == 0);
    assert(p == 10.5 && c == 10 && o == 9.75);
    assert(stock_parse_chart("{\"regularMarketPrice\":1,\"chartPreviousClose\":2,\"open\":[null]}", &p, &c, &o) == 0 && o == 2);
    assert(stock_parse_chart("{\"chart\":{\"result\":null,\"error\":{\"code\":\"Not Found\"}}}", &p, &c, &o) != 0);
    assert(stock_parse_chart("Too Many Requests", &p, &c, &o) != 0);
    for (int i = 1; i < argc; i++) {
        static char buf[16384];
        FILE *f = fopen(argv[i], "r");
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = 0;
        fclose(f);
        int rc = stock_parse_chart(buf, &p, &c, &o);
        printf("%s rc=%d price=%.2f prev=%.2f open=%.2f chg=%+.2f%%\n", argv[i], rc, p, c, o,
               rc ? 0 : (p - c) / c * 100);
    }
    puts("parser unit asserts OK");
    return 0;
}
