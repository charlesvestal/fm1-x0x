#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Compile one C file for the FM-1 (-mcpu=r3 -mfprev1 -O2) and report what the DSP
# rules forbid: calls into soft-double routines (__*df*), libm, malloc/free, and any
# undefined symbol outside an allow list. Also prints the instruction count of each
# function (a rough cost signal; tests/target_budget.py does the loop-weighted one).
#   tools/target_obj_check.sh firmware/src/dsp/bass303.c [extra cflags...]
# Needs JIELI_TOOLCHAIN (tools/get_toolchain.sh) and docker or podman.
set -e
cd "$(dirname "$0")/.."
SRCF="$1"; shift
: "${JIELI_TOOLCHAIN:?set JIELI_TOOLCHAIN}"
TC="$(cd "$JIELI_TOOLCHAIN" && pwd -P)"
DOCKER="$(command -v docker || command -v podman)"
OBJ="build/objcheck/$(basename "$SRCF" .c).o"
mkdir -p build/objcheck
run() { "$DOCKER" run --rm --platform linux/amd64 -v "$PWD:/work" -v "$TC:/opt/jieli:ro" -w /work \
        debian:bookworm-slim "$@"; }
run /opt/jieli/pi32v2/bin/clang -target pi32v2 -mcpu=r3 -mfprev1 -O2 -fno-builtin -ffp-contract=off \
    -Wall -Wdouble-promotion -Ifirmware/src -Ibuild/gen "$@" -c "$SRCF" -o "$OBJ"
run sh -c "/opt/jieli/pi32v2/bin/nm -u $OBJ; echo ---; /opt/jieli/common/bin/objdump -d $OBJ" > "$OBJ.txt"
UNDEF="$(awk '/^---$/{exit} {print $NF}' "$OBJ.txt")"
BAD=""
for s in $UNDEF; do
    case "$s" in
        __*df*|__*sf2df*|__extendsfdf2|__truncdfsf2) BAD="$BAD $s(double)";;
        malloc|calloc|realloc|free) BAD="$BAD $s(alloc)";;
        expf|exp|tanhf|tanh|powf|pow|sinf|sin|cosf|cos|tanf|tan|logf|log|log2f|log10f|sqrtf|sqrt|floorf|floor|fabsf|fabs|fmodf|fmod|exp2f|roundf|lrintf|atanf|atan2f|ceilf)
            BAD="$BAD $s(libm)";;
        memset|memcpy|memmove|memcmp) ;;      # the firmware's libc.c provides these
        __divsi3|__udivsi3|__modsi3|__umodsi3|__ashldi3|__lshrdi3|__ashrdi3|__muldi3|__udivdi3|__divdi3|__umoddi3|__moddi3) ;;
        *) BAD="$BAD $s(undefined)";;
    esac
done
echo "functions (instructions):"
awk '/^[A-Za-z_][A-Za-z0-9_]*:$/{if(f)printf "  %-40s %d\n",f,n; f=substr($0,1,length($0)-1); n=0; next} /^ +[0-9a-f]+:/{n++} END{if(f)printf "  %-40s %d\n",f,n}' "$OBJ.txt" | sort -k2 -n -r | head -40
if [ -n "$BAD" ]; then echo "FORBIDDEN:$BAD"; exit 1; fi
echo "ok: no double, libm, alloc or unknown symbols in $SRCF"
