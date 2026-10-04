#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build the GitHub Pages site for release VERSION (tools/make_pages.py) and push it to the gh-pages
# branch of origin, replacing what was there (the branch holds only the generated site).
#   tools/publish_pages.sh 0.1-beta          (after ./build.sh --release 0.1-beta)
set -e
cd "$(dirname "$0")/.."
V="${1:?usage: tools/publish_pages.sh VERSION}"
PKG="build/x0x-$V.fwsc"
[ -f "$PKG" ] || { echo "no $PKG: run ./build.sh --release $V first"; exit 1; }
python3 tools/make_pages.py "$PKG" "$V" build/pages
REMOTE="$(git remote get-url origin)"
SHA="$(git rev-parse --short HEAD)"
cd build/pages
rm -rf .git
git init -q -b gh-pages
git add -A
git commit -q -m "Site for X0X $V (from $SHA)"
git push -q -f "$REMOTE" gh-pages
rm -rf .git
echo "pushed the site for $V to gh-pages"
