// test_battery.c — host unit test for axp2101_decode() (pure, no ESP/LVGL).
//   gcc -std=c11 -Wall -Wextra -Ifirmware/main \
//       tests/test_battery.c firmware/main/axp2101_decode.c -o tb.exe && ./tb.exe
#include "axp2101_decode.h"
#include <stdio.h>
#include <stdlib.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...)                                            \
    do {                                                            \
        if (cond) { g_pass++; }                                     \
        else { g_fail++; printf("  FAIL (%s:%d): ", __FILE__, __LINE__); \
               printf(__VA_ARGS__); printf("\n"); }                 \
    } while (0)

static battery_status_t dec(uint8_t s1, uint8_t s2, uint8_t soc)
{
    battery_status_t b;
    axp2101_decode(s1, s2, soc, &b);
    return b;
}

int main(void)
{
    printf("# present flag = status1 bit3\n");
    CHECK(dec(0x08, 0, 50).present == true,  "bit3 set -> present");
    CHECK(dec(0x00, 0, 50).present == false, "bit3 clear -> absent");

    printf("# charging = status2 bits[6:5] == 0b01\n");
    CHECK(dec(0x08, 0x20, 50).charging == true,  "0x20: bits[6:5]=01 -> charging");
    CHECK(dec(0x08, 0x40, 50).charging == false, "0x40: bits[6:5]=10 -> discharging");
    CHECK(dec(0x08, 0x00, 50).charging == false, "0b00 -> not charging");
    CHECK(dec(0x00, 0x20, 50).charging == false, "charge bits set but absent -> not charging");

    printf("# percent passthrough + clamp 0..100\n");
    CHECK(dec(0x08, 0, 73).percent == 73,  "soc 73 -> 73");
    CHECK(dec(0x08, 0, 0).percent  == 0,   "soc 0 -> 0");
    CHECK(dec(0x08, 0, 100).percent== 100, "soc 100 -> 100");
    CHECK(dec(0x08, 0, 200).percent== 100, "soc 200 (garbage) -> clamped 100");

    printf("# external (VBUS) = status1 bit5\n");
    CHECK(dec(0x20, 0, 50).external == true,  "bit5 set -> external power");
    CHECK(dec(0x00, 0, 50).external == false, "bit5 clear -> no external power");
    CHECK(dec(0x08, 0, 50).external == false, "battery present, no VBUS -> not external");
    CHECK(dec(0x28, 0, 50).external == true,  "battery + VBUS -> external");

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
