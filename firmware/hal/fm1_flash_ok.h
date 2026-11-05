/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Laurens (@profikid) */
/* Which flash parts the FM-1's storage may use (main_fm1.c's boot: flash_ok).
 *
 * The stock FM-1 carries a Puya P25Q80 (JEDEC 85 60 14, 1 MiB). Units sold since autumn 2026 (mostly
 * AliExpress) carry a UC25HQ80 (JEDEC B3 60 14, 1 MiB, the same plain SPI NOR commands) instead.
 * A firmware that only knows the Puya takes those units for unknown: its saves stay in RAM, backup and
 * restore are refused and its update loader is never started.
 *
 * A copy of fm1-base's device/fm1_flash_ok.h (fm1-base#35, Laurens). Any other answer: RAM only,
 * nothing written. */
#ifndef FM1B_FLASH_OK_H
#define FM1B_FLASH_OK_H

/* the Puya P25Q80 (the stock part) and the UC25HQ80 (the newer units' part): both 1 MiB */
#ifndef FM1B_FLASH_JEDEC_OK
#define FM1B_FLASH_JEDEC_OK(id) ((id) == 0x856014u || (id) == 0xB36014u)
#endif

#endif
