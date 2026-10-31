#!/bin/bash
# FM-1 rescue: one command that puts M-VAVE's stock firmware back on an FM-1 stuck in a crash loop (macOS, Linux).
#   bash -c "$(curl -fsSL https://raw.githubusercontent.com/charlesvestal/fm1-x0x/main/tools/fm1_rescue.sh)"
set -e
DIR="$HOME/fm1-rescue"
RAW="https://raw.githubusercontent.com/charlesvestal/fm1-x0x/main/tools"
# M-VAVE's FM-1 firmware: V16 from M-VAVE, V15 (no longer there) from the Internet Archive. The script writes the
# one whose bootloader the FM-1 already has, so it is given both
V15=db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a
V16=d017fb65e70aca97ee29d944470c3515a74581df5c0576bf70de917c7b2b61d5
MVAVE="https://yms-file-store.oss-cn-hongkong.aliyuncs.com/software/firmware/FM-1.fwsc"
URL15="https://web.archive.org/web/20261009203224id_/$MVAVE"
sha() { if command -v shasum >/dev/null; then shasum -a 256 "$1"; else sha256sum "$1"; fi | cut -d' ' -f1; }
echo
echo "=== FM-1 rescue ==="
if [ "$(uname)" = Darwin ] && ! xcode-select -p >/dev/null 2>&1; then
    echo "This needs Apple's command line tools (free). A window will open: click Install."
    echo "When it has finished, run this same command again."
    xcode-select --install >/dev/null 2>&1 || true
    exit 1
fi
mkdir -p "$DIR"
cd "$DIR"
if [ ! -x env/bin/python ] || ! env/bin/python -c "import usb, libusb_package" 2>/dev/null; then
    echo "Setting up (once) ..."
    rm -rf env
    if [ "$(uname)" = Darwin ]; then PY=/usr/bin/python3; else PY=python3; fi
    if ! "$PY" -m venv env; then
        echo "Python's venv is missing: install it (on Ubuntu: sudo apt install python3-venv) and run this again."
        exit 1
    fi
    env/bin/pip install --quiet --disable-pip-version-check pyusb libusb-package
fi
curl -fsSL -o fm1_rescue.py "$RAW/fm1_rescue.py"
# each version: a copy already here, in Downloads or on the Desktop (found by its sha256), else downloaded
get() {
    local name=$1 want=$2 url=$3 f
    for f in "$DIR/FM-1-$name.fwsc" "$DIR"/*.fwsc "$HOME"/Downloads/*.fwsc "$HOME"/Desktop/*.fwsc; do
        [ -f "$f" ] || continue
        if [ "$(sha "$f")" = "$want" ]; then
            [ "$f" = "$DIR/FM-1-$name.fwsc" ] || cp "$f" "$DIR/FM-1-$name.fwsc"
            return 0
        fi
    done
    echo "Downloading the stock firmware (M-VAVE's FM-1 $name) ..."
    curl -fsSL -o "$DIR/FM-1.download" "$url" || true
    if [ "$(sha "$DIR/FM-1.download" 2>/dev/null)" != "$want" ]; then
        rm -f "$DIR/FM-1.download"
        echo "  (could not get $name: not the expected file)"
        return 1
    fi
    mv "$DIR/FM-1.download" "$DIR/FM-1-$name.fwsc"
}
PKGS=()
get V16 "$V16" "$MVAVE" && PKGS+=("$DIR/FM-1-V16.fwsc")
get V15 "$V15" "$URL15" && PKGS+=("$DIR/FM-1-V15.fwsc")
if [ ${#PKGS[@]} -eq 0 ]; then
    echo "Could not get M-VAVE's firmware. Nothing was done; send this output."
    exit 1
fi
echo "Stock firmware: ${PKGS[*]}"
echo
echo "If it asks for a password, type your computer's password (nothing shows while you type) and press Return."
echo "When it says it is waiting: plug the FM-1 in and switch it on."
echo "It will crash and go blank; that is when it is caught. (Ctrl-C stops it.)"
echo
exec sudo "$DIR/env/bin/python" "$DIR/fm1_rescue.py" "${PKGS[@]}" --ask --wait 300
