// test_conn.c — host unit test for the pure connection-state FSM (no ESP/LVGL).
//   cc -std=c11 -I../firmware/main test_conn.c ../firmware/main/conn.c -o test_conn
#include "conn.h"
#include <stdio.h>
#include <stdlib.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...)                                                \
    do {                                                               \
        if (cond) { g_pass++; }                                        \
        else { g_fail++; printf("  FAIL (%s:%d): ", __FILE__, __LINE__); \
               printf(__VA_ARGS__); printf("\n"); }                    \
    } while (0)

int main(void)
{
    printf("# boot starts CONNECTING and holds until grace\n");
    conn_init(15000, 0);
    CHECK(conn_get() == CONN_CONNECTING, "boot connecting (got %d)", conn_get());
    CHECK(conn_tick(0) == CONN_CONNECTING, "t=0 connecting");
    CHECK(conn_tick(14999) == CONN_CONNECTING, "just under grace connecting");

    printf("# frame before grace -> ONLINE, stays online forever\n");
    conn_init(15000, 0);
    conn_event(CONN_EV_LINK_UP, 3000);
    CHECK(conn_tick(3000) == CONN_ONLINE, "frame -> online");
    CHECK(conn_tick(999999) == CONN_ONLINE, "online never times out");

    printf("# no frame past grace -> OFFLINE and latches\n");
    conn_init(15000, 0);
    CHECK(conn_tick(15000) == CONN_OFFLINE, "at grace -> offline");
    CHECK(conn_tick(20000) == CONN_OFFLINE, "stays offline");

    printf("# drop -> reconnecting, times out to OFFLINE from the drop\n");
    conn_init(15000, 0);
    conn_event(CONN_EV_LINK_UP, 2000);
    conn_event(CONN_EV_LINK_DOWN, 5000);
    CHECK(conn_get() == CONN_CONNECTING, "drop -> connecting");
    CHECK(conn_tick(19999) == CONN_CONNECTING, "within grace after drop");
    CHECK(conn_tick(20000) == CONN_OFFLINE, "past grace after drop -> offline");

    printf("# reconnect within grace -> ONLINE (no offline flash)\n");
    conn_init(15000, 0);
    conn_event(CONN_EV_LINK_UP, 2000);
    conn_event(CONN_EV_LINK_DOWN, 5000);
    conn_event(CONN_EV_LINK_UP, 8000);
    CHECK(conn_tick(30000) == CONN_ONLINE, "reconnected -> online");

    printf("# grace is measured from the drop, not from boot\n");
    conn_init(15000, 0);
    conn_event(CONN_EV_LINK_UP, 1000);
    conn_event(CONN_EV_LINK_DOWN, 100000);   // long session, then drop
    CHECK(conn_tick(100000) == CONN_CONNECTING, "just dropped, connecting");
    CHECK(conn_tick(114999) == CONN_CONNECTING, "within grace from drop");
    CHECK(conn_tick(115000) == CONN_OFFLINE, "grace from drop -> offline");

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
