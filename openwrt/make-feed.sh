#!/bin/sh
#
# Assemble an OpenWrt package feed containing this source tree.
#
#   ./openwrt/make-feed.sh [output-dir]      (default: openwrt/feed)
#
# Produces <output-dir>/net/asterisk-chan-quectel/ holding the package Makefile
# and a copy of the sources, plus <output-dir>/../feeds-quectel.conf to point an
# SDK at it.
#
# Versioning
# ----------
# Everything comes from the most recent git tag, so a release needs nothing but
# a tag. PKG_RELEASE carries the distance from that tag, which is what it is
# for - a packaging iteration on top of the same upstream version:
#
#   exactly on tag v0.1.0    -> 0.1.0        (no -r suffix at all)
#   5 commits after v0.1.0   -> 0.1.0-r5
#   dirty working tree       -> 0.1.0-r<unix timestamp>
#
# OpenWrt omits the -r entirely when PKG_RELEASE is empty (package-defaults.mk),
# and apk orders these correctly: 0.1.0 < 0.1.0-r5 < 0.1.0-r<ts> < 0.2.0.
#
# The timestamp case matters because apk silently refuses to reinstall a package
# whose version already matches what is installed, so an uncommitted rebuild
# would otherwise never deploy.
#
# apk is picky about the format - only digits and dots in PKG_VERSION. A suffix
# such as "~g1a2b3c4" is rejected because it starts with a letter.
#
set -e

SRC=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$SRC/openwrt/feed}
PKGDIR=$OUT/net/asterisk-chan-quectel

PKG_VERSION=0.0.0
PKG_RELEASE=
VERSION_STRING=0.0.0
DIRTY=1
AHEAD=0

if command -v git >/dev/null 2>&1 && git -C "$SRC" rev-parse --git-dir >/dev/null 2>&1; then
    [ -z "$(git -C "$SRC" status --porcelain 2>/dev/null)" ] && DIRTY=

    VERSION_STRING=$(git -C "$SRC" describe --always --tags --dirty 2>/dev/null || echo "0.0.0")
    DESC=$(git -C "$SRC" describe --tags --always --match 'v[0-9]*' 2>/dev/null || echo "")
    case "$DESC" in
        v*)
            BASE=${DESC#v}
            PKG_VERSION=${BASE%%-*}
            case "$BASE" in
                *-*-g*) AHEAD=$(echo "$BASE" | sed -n 's/^[0-9.]*-\([0-9]*\)-g.*$/\1/p') ;;
            esac
            ;;
        *)
            echo "note: no v* tag found, using $PKG_VERSION"
            ;;
    esac
fi

if [ -n "$DIRTY" ]; then
    PKG_RELEASE=$(date +%s)
    echo "note: working tree is dirty or not a git repo -"
    echo "      using PKG_RELEASE=$PKG_RELEASE so apk always sees a newer version"
elif [ -n "$AHEAD" ] && [ "$AHEAD" != "0" ]; then
    PKG_RELEASE=$AHEAD
fi

rm -rf "$PKGDIR"
mkdir -p "$PKGDIR/src/src"

sed -e "s|@PKG_VERSION@|$PKG_VERSION|" \
    -e "s|@PKG_RELEASE@|$PKG_RELEASE|" \
    -e "s|@VERSION_STRING@|$VERSION_STRING|" \
    "$SRC/openwrt/Makefile.in" > "$PKGDIR/Makefile"

# The package builds with cmake.mk, so the CMake project must sit at the root
# of the bundled source tree.
cp "$SRC/CMakeLists.txt" "$SRC/quectel.conf" "$SRC/README.md" "$PKGDIR/src/"
cp "$SRC"/src/*.c "$SRC"/src/*.h "$PKGDIR/src/src/"

mkdir -p "$OUT"
printf 'src-link quectel %s\n' "$OUT" > "$SRC/openwrt/feeds-quectel.conf"

echo "feed    : $PKGDIR"
if [ -n "$PKG_RELEASE" ]; then
    echo "version : ${PKG_VERSION}-r${PKG_RELEASE}"
else
    echo "version : ${PKG_VERSION}"
fi
echo
echo "To build it, from an extracted OpenWrt SDK:"
echo "  cp $SRC/openwrt/feeds-quectel.conf ."
echo "  cat feeds.conf.default feeds-quectel.conf > feeds.conf"
echo "  ./scripts/feeds update -a && ./scripts/feeds install asterisk-chan-quectel"
echo "  make defconfig"
echo "  make package/asterisk-chan-quectel/compile V=s"
