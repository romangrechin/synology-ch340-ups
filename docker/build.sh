#!/bin/sh
# Builds the .spk inside the container. Repo is mounted at /src, output goes
# to /src/build. Driven by TARGET_ARCH, SPK_ARCH, HOST_UID, HOST_GID.
set -eu

SRC=/src
NUT=/tmp/nut
ROOT=/tmp/spk_root
PAY=/tmp/payload

TARGET_ARCH=${TARGET_ARCH:-x86_64}
VERSION=$(cat "$SRC/VERSION")

case "$TARGET_ARCH" in
x86_64)
	TRIPLE=""
	DEFAULT_SPK_ARCH="x86_64"
	;;
aarch64)
	TRIPLE="aarch64-linux-gnu"
	DEFAULT_SPK_ARCH="rtd1296"
	;;
armv7)
	TRIPLE="arm-linux-gnueabihf"
	DEFAULT_SPK_ARCH="armada38x"
	;;
*)
	echo "unsupported TARGET_ARCH '$TARGET_ARCH' (x86_64, aarch64, armv7)" >&2
	exit 2
	;;
esac

SPK_ARCH=${SPK_ARCH:-$DEFAULT_SPK_ARCH}

if [ -n "$TRIPLE" ]; then
	CC="$TRIPLE-gcc"
	STRIP="$TRIPLE-strip"
	CONFIGURE_HOST="--host=$TRIPLE"
else
	CC=gcc
	STRIP=strip
	CONFIGURE_HOST=""
fi

echo "CH340 USB UPS Driver $VERSION | target $TARGET_ARCH | arch=\"$SPK_ARCH\""

if [ "$SPK_ARCH" != "x86_64" ]; then
	echo "NOTE: arch=\"$SPK_ARCH\" is a guess; override with --spk-arch if your model differs."
fi

if [ ! -f "$SRC/external/nut/configure.ac" ]; then
	echo "NUT submodule is empty. Run: git submodule update --init --recursive" >&2
	exit 1
fi

# --- sources ---------------------------------------------------------------

echo "==> preparing NUT source"
rm -rf "$NUT"
cp -a "$SRC/external/nut" "$NUT"
rm -rf "$NUT/.git"

# Windows clones may carry CRLF, which breaks patch(1).
for f in $(grep -h '^+++ b/' "$SRC"/patches/*.patch | sed 's|^+++ b/||' | sort -u); do
	[ -f "$NUT/$f" ] && sed -i 's/\r$//' "$NUT/$f"
done

for p in "$SRC"/patches/*.patch; do
	patch -p1 -s -d "$NUT" < "$p"
	echo "    applied $(basename "$p")"
done

# --- driver ----------------------------------------------------------------

echo "==> autogen"
cd "$NUT"
./autogen.sh >/tmp/autogen.log 2>&1 || { tail -20 /tmp/autogen.log >&2; exit 1; }

echo "==> configure"
# Paths must match DSM's own NUT build or upsd will not find the driver.
./configure $CONFIGURE_HOST \
	--prefix=/usr \
	--sysconfdir=/etc/ups \
	--with-drivers=blazer_ser \
	--with-statepath=/run/ups_state \
	--with-user=root --with-group=root \
	--without-usb --without-snmp --without-neon --without-libltdl \
	--without-ipmi --without-freeipmi --without-powerman \
	--without-cgi --without-dev --without-doc \
	--disable-shared \
	CC="$CC" LDFLAGS="-static" >/tmp/configure.log 2>&1 ||
	{ tail -25 /tmp/configure.log >&2; exit 1; }

echo "==> building blazer_ser"
make -C include >/dev/null 2>&1 || true
make -C common >/tmp/common.log 2>&1 || { tail -20 /tmp/common.log >&2; exit 1; }
make -C drivers blazer_ser >/tmp/driver.log 2>&1 || { tail -25 /tmp/driver.log >&2; exit 1; }
grep -E 'blazer_ser\.c.*(warning|error)' /tmp/driver.log || true

rm -rf "$PAY"
mkdir -p "$PAY/bin"
cp "$NUT/drivers/blazer_ser" "$PAY/blazer_ser"
"$STRIP" "$PAY/blazer_ser"

# --- helper ----------------------------------------------------------------

echo "==> building helper"
# Pinned hash: the helper installs no driver but this one.
SHA=$(sha256sum "$PAY/blazer_ser" | cut -d' ' -f1)
"$CC" -static -O2 -Wall -Wextra -o "$PAY/bin/ch340ups-apply" \
	"$SRC/helper/ch340ups-apply.c" -DDRIVER_SHA256="\"$SHA\""

# --- package ---------------------------------------------------------------

echo "==> assembling package"
rm -rf "$ROOT"
mkdir -p "$ROOT/conf" "$ROOT/scripts"

sed -e "s|@VERSION@|$VERSION|g" -e "s|@ARCH@|$SPK_ARCH|g" \
	"$SRC/package/INFO.in" > "$ROOT/INFO"
cp "$SRC/package/conf/privilege" "$ROOT/conf/privilege"
cp "$SRC"/package/scripts/* "$ROOT/scripts/"
cp "$SRC/package/icons/PACKAGE_ICON.PNG" "$SRC/package/icons/PACKAGE_ICON_256.PNG" "$ROOT/"

for f in "$ROOT"/scripts/*; do
	sed -i 's/\r$//' "$f"
done

chmod 755 "$PAY/blazer_ser" "$PAY/bin/ch340ups-apply" "$ROOT"/scripts/*
chmod 644 "$ROOT/INFO" "$ROOT/conf/privilege" "$ROOT"/*.PNG

tar --format=ustar --owner=0 --group=0 --numeric-owner \
	-czf "$ROOT/package.tgz" -C "$PAY" blazer_ser bin

# DSM requires an uncompressed ustar with INFO first and no "./" prefix.
OUT="$SRC/build/CH340UPS-$VERSION-$SPK_ARCH.spk"
mkdir -p "$SRC/build"
tar --format=ustar --owner=0 --group=0 --numeric-owner \
	-cf "$OUT" -C "$ROOT" \
	INFO package.tgz scripts conf PACKAGE_ICON.PNG PACKAGE_ICON_256.PNG

if [ -n "${HOST_UID:-}" ] && [ -n "${HOST_GID:-}" ]; then
	chown -R "$HOST_UID:$HOST_GID" "$SRC/build"
fi

echo "built build/$(basename "$OUT") ($(wc -c < "$OUT") bytes)"
