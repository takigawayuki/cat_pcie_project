#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
KO="$PROJECT_DIR/driver/colorbar_pcie_driver.ko"
MODULE="colorbar_pcie_driver"
DEVICE="/dev/colorbar_pcie_rx"

has_param() {
    key="$1"
    shift
    for arg in "$@"; do
        case "$arg" in
            "$key"=*) return 0 ;;
        esac
    done
    return 1
}

PARAMS=""
if ! has_param bar "$@"; then
    PARAMS="$PARAMS bar=1"
fi
if ! has_param addr_byteswap "$@"; then
    PARAMS="$PARAMS addr_byteswap=1"
fi
if ! has_param frame_wait_ms "$@"; then
    PARAMS="$PARAMS frame_wait_ms=100"
fi
if ! has_param dma_len_bytes "$@"; then
    PARAMS="$PARAMS dma_len_bytes=64"
fi
if ! has_param verify_readback "$@"; then
    PARAMS="$PARAMS verify_readback=1"
fi

if [ ! -f "$KO" ]; then
    echo "driver ko not found: $KO" >&2
    echo "run: make" >&2
    exit 1
fi

if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
else
    SUDO="sudo"
fi

$SUDO rmmod "$MODULE" 2>/dev/null || true
# shellcheck disable=SC2086
$SUDO insmod "$KO" $PARAMS "$@"

if [ -e "$DEVICE" ]; then
    echo "device ready: $DEVICE"
else
    echo "warning: $DEVICE not found" >&2
fi

echo "loaded params: $PARAMS $*"
echo "recent driver log:"
$SUDO dmesg | tail -n 20
