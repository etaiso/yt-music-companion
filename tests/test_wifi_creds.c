// test_wifi_creds.c — host unit test for the pure credential predicate.
//   cc -std=c11 -I../firmware/main test_wifi_creds.c ../firmware/main/wifi_creds.c -o test_wifi_creds
#include "wifi_creds.h"
#include <stdio.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...)                                                     \
    do { if (cond) { g_pass++; }                                            \
         else { g_fail++; printf("  FAIL (%s:%d): ", __FILE__, __LINE__);   \
                printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    CHECK(ytm_creds_valid("Koko2.4"),  "a real SSID is valid");
    CHECK(!ytm_creds_valid(""),        "empty SSID is invalid");
    CHECK(!ytm_creds_valid("changeme"),"placeholder is invalid");
    CHECK(!ytm_creds_valid(0),         "NULL is invalid");
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
