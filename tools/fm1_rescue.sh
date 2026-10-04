#!/bin/bash
# FM-1 rescue: one command that puts M-VAVE's stock firmware back on an FM-1 stuck in a crash loop.
#   bash -c "$(curl -fsSL https://raw.githubusercontent.com/charlesvestal/fm1-x0x/main/tools/fm1_rescue.sh)"
set -e
DIR="$HOME/fm1-rescue"
RAW="https://raw.githubusercontent.com/charlesvestal/fm1-x0x/main/tools"
STOCK=db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a
echo
echo "=== FM-1 rescue ==="
if ! xcode-select -p >/dev/null 2>&1; then
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
    /usr/bin/python3 -m venv env
    env/bin/pip install --quiet --disable-pip-version-check pyusb libusb-package
fi
curl -fsSL -o fm1_rescue.py "$RAW/fm1_rescue.py"
PKG=""
for f in "$DIR"/*.fwsc "$HOME"/Downloads/*.fwsc "$HOME"/Desktop/*.fwsc; do
    [ -f "$f" ] || continue
    if [ "$(shasum -a 256 "$f" | cut -d' ' -f1)" = "$STOCK" ]; then PKG="$f"; break; fi
done
if [ -z "$PKG" ]; then
    echo "Downloading the stock firmware (M-VAVE's FM-1 V15) ..."
    curl -fsSL -o "$DIR/FM-1.download" "https://yms-file-store.oss-cn-hongkong.aliyuncs.com/software/firmware/FM-1.fwsc"
    if [ "$(shasum -a 256 "$DIR/FM-1.download" | cut -d' ' -f1)" != "$STOCK" ]; then
        echo "The downloaded file is not the expected V15 firmware. Nothing was done; send this output."
        exit 1
    fi
    mv "$DIR/FM-1.download" "$DIR/FM-1.fwsc"
    PKG="$DIR/FM-1.fwsc"
fi
cp "$PKG" "$DIR/FM-1.fwsc" 2>/dev/null || true
echo "Stock firmware: $PKG"
echo
echo "Next: your Mac password (nothing shows while you type it)."
echo "Then plug the FM-1 in and switch it on. It will crash and go blank; that is when it is caught."
echo
exec sudo "$DIR/env/bin/python" "$DIR/fm1_rescue.py" "$DIR/FM-1.fwsc" --ask --wait 300
