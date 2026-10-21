#!/bin/sh
# Builds the .spk in Docker. Works on Linux, macOS and Git Bash on Windows.
# Usage: ./build.sh [x86_64|aarch64|armv7] [--spk-arch VALUE]
set -eu

IMAGE=ch340ups-builder
ARCH=x86_64
SPK_ARCH=""

usage() {
	echo "usage: ./build.sh [x86_64|aarch64|armv7] [--spk-arch VALUE]"
	exit "${1:-0}"
}

while [ $# -gt 0 ]; do
	case "$1" in
	x86_64 | aarch64 | armv7)
		ARCH=$1
		;;
	--spk-arch)
		shift
		SPK_ARCH=${1:-}
		[ -n "$SPK_ARCH" ] || usage 2
		;;
	-h | --help)
		usage
		;;
	*)
		echo "unknown argument: $1" >&2
		usage 2
		;;
	esac
	shift
done

cd "$(dirname "$0")"

case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*)
	# Docker needs a Windows path, and MSYS must not rewrite it
	MOUNT=$(pwd -W)
	MSYS_NO_PATHCONV=1
	export MSYS_NO_PATHCONV
	OWNER=""
	;;
*)
	MOUNT=$(pwd)
	OWNER="-e HOST_UID=$(id -u) -e HOST_GID=$(id -g)"
	;;
esac

docker build -t "$IMAGE" .
# shellcheck disable=SC2086
docker run --rm -v "$MOUNT:/src" \
	-e TARGET_ARCH="$ARCH" ${SPK_ARCH:+-e SPK_ARCH="$SPK_ARCH"} $OWNER \
	"$IMAGE"
