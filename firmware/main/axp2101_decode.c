// axp2101_decode.c — implementation of axp2101_decode().
// See axp2101_decode.h for register documentation.
#include "axp2101_decode.h"

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void axp2101_decode(uint8_t status1, uint8_t status2, uint8_t soc_pct,
                    battery_status_t *out)
{
    out->present  = (status1 & (1u << 3)) != 0u;
    out->charging = (((status2 >> 5) & 0x3u) == 0x1u);
    out->percent  = clampi((int)soc_pct, 0, 100);
}
