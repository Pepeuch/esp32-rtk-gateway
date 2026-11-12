#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
output="$(mktemp -d)"
trap 'rm -rf "$output"' EXIT
common=(
  -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined
  -I"$root/tests/host/stubs" -I"$root/components/espnow_rtcm/include" \
  -I"$root/components/lora_transport/include" -I"$root/components/rtcm_filter/include" \
  "$root/components/espnow_rtcm/src/espnow_rtcm_codec.c" \
  "$root/components/lora_transport/src/lora_transport.c" \
  "$root/components/rtcm_filter/src/rtcm_filter.c" \
  "$root/components/rtcm_filter/src/rtcm_profiles.c"
)
cc "${common[@]}" "$root/tests/host/test_espnow_rtcm.c" -o "$output/codec"
"$output/codec"
cc "${common[@]}" "$root/tests/host/bridge_pipe.c" -o "$output/bridge"
ESPNOW_BRIDGE_TEST_EXE="$output/bridge" PYTHONDONTWRITEBYTECODE=1 \
  python3 -m unittest discover -s "$root/tests/host" -p 'test_rtcm_usb.py' -v
