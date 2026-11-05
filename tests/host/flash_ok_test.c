/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Laurens (@profikid) */
/* firmware/hal/fm1_flash_ok.h: the flash parts the FM-1's storage may use (fm1-base#35's test).
 *
 * The stock part (the Puya P25Q80) and the newer units' part (the UC25HQ80) are accepted; anything else
 * is not: the device then runs from RAM and writes nothing. */
#include <stdio.h>

#include "../../firmware/hal/fm1_flash_ok.h"

static int fails;

static void check(const char *name, int got, int want)
{
    printf("  %-24s %s\n", name, got == want ? "ok" : "FAIL");
    if (got != want)
        fails++;
}

int main(void)
{
    check("Puya P25Q80", FM1B_FLASH_JEDEC_OK(0x856014u), 1);
    check("UC25HQ80", FM1B_FLASH_JEDEC_OK(0xB36014u), 1);
    check("no part answered", FM1B_FLASH_JEDEC_OK(0u), 0);
    check("all ones", FM1B_FLASH_JEDEC_OK(0xFFFFFFu), 0);
    check("Winbond 4 MiB", FM1B_FLASH_JEDEC_OK(0xEF4016u), 0);
    check("GigaDevice", FM1B_FLASH_JEDEC_OK(0xC84019u), 0);
    check("near-miss Puya", FM1B_FLASH_JEDEC_OK(0x856015u), 0);
    check("near-miss UC25", FM1B_FLASH_JEDEC_OK(0xB36015u), 0);
    printf("%s\n", fails ? "FLASH JEDEC FAILED" : "flash jedec ok");
    return fails ? 1 : 0;
}
