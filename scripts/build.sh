#!/bin/sh
# Build the perseus kernel (and optionally the loader) from a HackiOS checkout.
#
#   HACKIOS=/path/to/hackios-xnu-kernel sh scripts/build.sh
#   HACKIOS=... BUILD_LOADER=1 sh scripts/build.sh
#
# Needs, besides HACKIOS:
#   - clang, lld, python3, perl, xz, flex, bison, m4, git
#   - theos in $THEOS (or ~/theos) with the iPhoneOS16.5.sdk and cctools
#   - ~2 GB disk and a Linux host (no macOS, no Xcode)
set -eu

HERE=$(cd "$(dirname "$0")/.." && pwd)
: "${HACKIOS:?set HACKIOS to a hackios-xnu-kernel checkout}"

XNU_TAG_COMMIT=c54f35ca767986246321eb901baf8f5ff7923f6a
REDWOOD_SHA=ac3b2b578a6d1d480079557a576bf1bb4155082d4ee2958373c18f5d78b8e89b

say() { printf '\n== %s\n' "$*"; }

say "0. baseline"
[ -f "$HACKIOS/xnu/apply.sh" ] || { echo "$HACKIOS does not look like hackios-xnu-kernel" >&2; exit 1; }
got=$(sha256sum "$HACKIOS/xnu/redwood.patch" | cut -d' ' -f1)
if [ "$got" != "$REDWOOD_SHA" ]; then
	printf 'warning: redwood.patch is %s\n' "$got"
	printf '         this port was made against %s\n' "$REDWOOD_SHA"
	printf '         the patches below may not apply; continuing in 5 s\n'
	sleep 5
fi

say "1. XNU sources (Apple tag + redwood patch)"
if [ -d "$HACKIOS/xnu-src/xnu-8796.121.2" ]; then
	echo "xnu-src already there, left alone"
else
	( cd "$HACKIOS" && xnu/apply.sh )
fi
X="$HACKIOS/xnu-src/xnu-8796.121.2"
[ "$(git -C "$X" rev-parse HEAD)" = "$XNU_TAG_COMMIT" ] || echo "warning: XNU is not $XNU_TAG_COMMIT"

say "2. perseus patches"
cd "$X"
for p in "$HERE"/xnu/patches/*.patch; do
	echo "  $(basename "$p")"
	git apply --check "$p" || { echo "does not apply: $p" >&2; exit 1; }
	git apply "$p"
done
cp -a "$HERE/xnu/files/." .

say "3. build dependencies"
[ -d "$HACKIOS/xnu-deps/build" ] || ( cd "$HACKIOS" && xnu-deps/setup.sh )
[ -d "$HACKIOS/xnu-deps/host/bin" ] || ( cd "$HACKIOS" && xnu-deps/setup.sh )

say "4. kernel"
cd "$HACKIOS"
# make exits 2 after the kernel is linked (export check); do not treat it as a failure
./xnu-build.sh -j"${JOBS:-8}" -k || true
K="$X/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837"
[ -f "$K" ] || { echo "no kernel at $K" >&2; exit 1; }
ls -l "$K"
sha256sum "$K"

if [ "${BUILD_LOADER:-0}" = 1 ]; then
	say "5. loader"
	cd "$HACKIOS/xnuloader"
	if ! grep -q ramdisk1 loader.c; then
		git apply --check "$HERE/xnuloader/patches/0001-perseus-loader.patch" || {
			echo "loader patch does not apply (already patched?)" >&2; }
		git apply "$HERE/xnuloader/patches/0001-perseus-loader.patch" || true
	fi
	python3 "$HERE/device/add-platform.py" loader.c "$HERE/device/perseus.platform.c"
	./build.sh
	ls -l loader.efi
fi

say "done"
