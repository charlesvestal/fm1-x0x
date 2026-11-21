/* SPDX-License-Identifier: GPL-3.0-only */
/* The 303 drive's acid line (tools/drive_line.h) through dsp/bass303.c at one drive setting, for
 * tools/fit_drive_migration.py, which builds it twice: against this firmware's bass303.c and against
 * the one from before the drive was rescaled; and for tools/measure_drive_makeup.py.
 *   drive_render TYPE VOLUME DRIVE [PATCH]  TYPE: 1 Soft, 2 RAT; PATCH: an index into DRIVE_PATCHES
 *                                           (0); writes the line from step 16 (DRIVE_LINE_SEG0) on,
 *                                           where its levels are measured: the length n (int32),
 *                                           then n samples (float32)
 *   drive_render patches                    the patches' names, one a line, in order
 *   drive_render map TYPE                   (built with -DDRIVE_MAP, against this firmware's bass303.c)
 *                                           the migration of every old setting of that type: for each
 *                                           old Volume, each old Drive, a line "new Drive, new Volume" */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bass303.h"
#include "drive_line.h"

static bass303_t B;
static float out[DRIVE_LINE_BUF];

int main(int argc, char **argv)
{
#ifdef DRIVE_MAP
    if (argc == 3 && !strcmp(argv[1], "map")) {
        for (int v = 0; v < 128; v++)
            for (int d = 0; d < 128; d++) {
                int nd, nv;
                bass303_pre_rescale(atoi(argv[2]), v, d, &nd, &nv);
                printf("%d %d\n", nd, nv);
            }
        return 0;
    }
#endif
    if (argc == 2 && !strcmp(argv[1], "patches")) {
        for (int p = 0; p < DRIVE_NPATCH; p++)
            printf("%s\n", DRIVE_PATCHES[p].name);
        return 0;
    }
    if (argc != 4 && argc != 5) {
        fprintf(stderr, "usage: drive_render TYPE VOLUME DRIVE [PATCH] | drive_render patches\n");
        return 2;
    }
    const int pt = argc == 5 ? atoi(argv[4]) : 0;
    if (pt < 0 || pt >= DRIVE_NPATCH) {
        fprintf(stderr, "drive_render: PATCH 0..%d\n", DRIVE_NPATCH - 1);
        return 2;
    }
    const int drive = atoi(argv[3]), from = 0, n = DRIVE_LINE_LEN - DRIVE_LINE_SEG0;
    drive_line_render(&B, out, pt, atoi(argv[1]), atoi(argv[2]), &drive, &from, 1);
    fwrite(&n, sizeof n, 1, stdout);
    fwrite(out + DRIVE_LINE_SEG0, sizeof *out, (size_t)n, stdout);
    return 0;
}
