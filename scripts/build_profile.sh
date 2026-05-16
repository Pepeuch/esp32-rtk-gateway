#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 profile.json" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PROFILE_PATH="$1"
GENERATED_DIR="${ROOT_DIR}/build/generated"

mkdir -p "${GENERATED_DIR}"

PROFILE_ENV="${GENERATED_DIR}/profile.env"
GENERATED_DEFAULTS="${GENERATED_DIR}/sdkconfig.defaults"
GENERATED_PARTITIONS="${GENERATED_DIR}/partitions.csv"

python3 - "${PROFILE_PATH}" "${PROFILE_ENV}" <<'PY'
import json
import pathlib
import shlex
import sys

profile_path = pathlib.Path(sys.argv[1])
env_path = pathlib.Path(sys.argv[2])

if not profile_path.is_file():
    raise SystemExit(f"profile file not found: {profile_path}")

profile = json.loads(profile_path.read_text(encoding="utf-8"))
required = ["chip", "board", "role", "flash", "psram", "ethernet", "gnss", "lora", "region"]
missing = [key for key in required if key not in profile or profile[key] in ("", None)]
if missing:
    raise SystemExit("profile missing required fields: " + ", ".join(missing))

chip = str(profile["chip"]).lower()
board = str(profile["board"]).lower()
role = str(profile["role"]).lower()
flash = str(profile["flash"]).lower()
psram = str(profile["psram"]).lower()
ethernet = str(profile["ethernet"]).lower()
gnss = str(profile["gnss"]).lower()
lora = str(profile["lora"]).lower()
region = str(profile["region"]).upper()

board_rules = {
    "waveshare_esp32s3_eth": {"chip": "esp32s3", "default_ethernet": "w5500", "supports_lora": True},
    "mammotion_esp32s3": {"chip": "esp32s3", "default_ethernet": "none", "supports_lora": True},
    "generic_esp32_eth": {"chip": "esp32", "default_ethernet": "lan8720", "supports_lora": False},
}

if board not in board_rules:
    raise SystemExit(f"unsupported board: {board}")
if chip != board_rules[board]["chip"]:
    raise SystemExit(f"board {board} requires chip {board_rules[board]['chip']}, got {chip}")

if role not in {"base", "rover", "dual_debug"}:
    raise SystemExit(f"unsupported role: {role}")
if flash not in {"4mb", "8mb", "16mb"}:
    raise SystemExit(f"unsupported flash size: {flash}")
if psram not in {"none", "4mb", "8mb"}:
    raise SystemExit(f"unsupported psram size: {psram}")
if ethernet not in {"none", "w5500", "lan8720"}:
    raise SystemExit(f"unsupported ethernet profile: {ethernet}")
if gnss not in {"ublox", "nmea", "unicore", "septentrio"}:
    raise SystemExit(f"unsupported gnss profile: {gnss}")
if lora not in {"none", "sx126x"}:
    raise SystemExit(f"unsupported lora profile: {lora}")
if region not in {"EU868", "US915", "AU915", "AS923", "CUSTOM"}:
    raise SystemExit(f"unsupported region: {region}")

if role == "rover" and ethernet != "none":
    raise SystemExit("rover role requires ethernet=none")
if board == "generic_esp32_eth" and ethernet != "lan8720":
    raise SystemExit("generic_esp32_eth requires ethernet=lan8720")
if board == "waveshare_esp32s3_eth" and role != "rover" and ethernet != "w5500":
    raise SystemExit("waveshare_esp32s3_eth base/dual_debug profiles require ethernet=w5500")
if board == "mammotion_esp32s3" and ethernet != "none":
    raise SystemExit("mammotion_esp32s3 requires ethernet=none")
if not board_rules[board]["supports_lora"] and lora != "none":
    raise SystemExit(f"board {board} does not support lora={lora}")
if chip != "esp32s3" and psram == "8mb":
    raise SystemExit(f"chip {chip} does not support psram={psram} in this profile builder")

firmware_id = f"{board}-{role}-{region.lower()}-{gnss}-{lora}-{ethernet}-{flash}-{psram}"
partition_source = {
    "4mb": "partitions_4mb.csv",
    "8mb": "partitions_8mb.csv",
    "16mb": "partitions_16mb.csv",
}[flash]
target = chip

with env_path.open("w", encoding="utf-8") as fh:
    for key, value in [
        ("PROFILE_CHIP", chip),
        ("PROFILE_BOARD", board),
        ("PROFILE_ROLE", role),
        ("PROFILE_FLASH", flash),
        ("PROFILE_PSRAM", psram),
        ("PROFILE_ETHERNET", ethernet),
        ("PROFILE_GNSS", gnss),
        ("PROFILE_LORA", lora),
        ("PROFILE_REGION", region),
        ("FIRMWARE_ID", firmware_id),
        ("PARTITION_SOURCE", partition_source),
        ("IDF_TARGET_NAME", target),
    ]:
        fh.write(f"{key}={shlex.quote(value)}\n")
PY

# shellcheck disable=SC1090
source "${PROFILE_ENV}"

BUILD_DIR="${ROOT_DIR}/build/profile-${FIRMWARE_ID}"
SDKCONFIG_DEFAULTS_PATH="${ROOT_DIR}/build/generated/sdkconfig.defaults"
FIRMWARE_DIR="${ROOT_DIR}/docs/firmware/${FIRMWARE_ID}"
MANIFEST_PATH="${ROOT_DIR}/docs/manifests/${FIRMWARE_ID}.json"
PROFILE_COPY_PATH="${FIRMWARE_DIR}/profile.json"

ORIGINAL_PARTITIONS_BACKUP="${GENERATED_DIR}/partitions.csv.backup"
ORIGINAL_SDKCONFIG_BACKUP="${GENERATED_DIR}/sdkconfig.backup"
ORIGINAL_SDKCONFIG_OLD_BACKUP="${GENERATED_DIR}/sdkconfig.old.backup"
HAD_SDKCONFIG=0
HAD_SDKCONFIG_OLD=0
HAD_PARTITIONS=0

restore_workspace() {
    if [ "${HAD_PARTITIONS}" -eq 1 ] && [ -f "${ORIGINAL_PARTITIONS_BACKUP}" ]; then
        cp "${ORIGINAL_PARTITIONS_BACKUP}" "${ROOT_DIR}/partitions.csv"
    fi
    if [ "${HAD_PARTITIONS}" -eq 0 ]; then
        rm -f "${ROOT_DIR}/partitions.csv"
    fi

    if [ "${HAD_SDKCONFIG}" -eq 1 ] && [ -f "${ORIGINAL_SDKCONFIG_BACKUP}" ]; then
        cp "${ORIGINAL_SDKCONFIG_BACKUP}" "${ROOT_DIR}/sdkconfig"
    elif [ "${HAD_SDKCONFIG}" -eq 0 ]; then
        rm -f "${ROOT_DIR}/sdkconfig"
    fi

    if [ "${HAD_SDKCONFIG_OLD}" -eq 1 ] && [ -f "${ORIGINAL_SDKCONFIG_OLD_BACKUP}" ]; then
        cp "${ORIGINAL_SDKCONFIG_OLD_BACKUP}" "${ROOT_DIR}/sdkconfig.old"
    elif [ "${HAD_SDKCONFIG_OLD}" -eq 0 ]; then
        rm -f "${ROOT_DIR}/sdkconfig.old"
    fi
}

trap restore_workspace EXIT

if [ -f "${ROOT_DIR}/partitions.csv" ]; then
    cp "${ROOT_DIR}/partitions.csv" "${ORIGINAL_PARTITIONS_BACKUP}"
    HAD_PARTITIONS=1
fi

if [ -f "${ROOT_DIR}/sdkconfig" ]; then
    cp "${ROOT_DIR}/sdkconfig" "${ORIGINAL_SDKCONFIG_BACKUP}"
    HAD_SDKCONFIG=1
fi

if [ -f "${ROOT_DIR}/sdkconfig.old" ]; then
    cp "${ROOT_DIR}/sdkconfig.old" "${ORIGINAL_SDKCONFIG_OLD_BACKUP}"
    HAD_SDKCONFIG_OLD=1
fi

mkdir -p "${BUILD_DIR}" "${FIRMWARE_DIR}" "$(dirname "${MANIFEST_PATH}")"
cp "${ROOT_DIR}/${PARTITION_SOURCE}" "${GENERATED_PARTITIONS}"
cp "${GENERATED_PARTITIONS}" "${ROOT_DIR}/partitions.csv"
cp "${PROFILE_PATH}" "${PROFILE_COPY_PATH}"

cat /dev/null > "${GENERATED_DEFAULTS}"

append_template() {
    local template="$1"
    local template_path="${ROOT_DIR}/configs/templates/${template}"
    if [ ! -f "${template_path}" ]; then
        echo "Missing template: ${template_path}" >&2
        exit 1
    fi
    printf "# %s\n" "${template}" >> "${GENERATED_DEFAULTS}"
    cat "${template_path}" >> "${GENERATED_DEFAULTS}"
    printf "\n" >> "${GENERATED_DEFAULTS}"
}

append_template "sdkconfig.common.defaults"
append_template "sdkconfig.${PROFILE_CHIP}.defaults"
append_template "sdkconfig.flash_${PROFILE_FLASH}.defaults"
append_template "sdkconfig.psram_${PROFILE_PSRAM}.defaults"
append_template "sdkconfig.board_${PROFILE_BOARD}.defaults"
append_template "sdkconfig.role_${PROFILE_ROLE}.defaults"
append_template "sdkconfig.eth_${PROFILE_ETHERNET}.defaults"
append_template "sdkconfig.gnss_${PROFILE_GNSS}.defaults"
append_template "sdkconfig.lora_${PROFILE_LORA}.defaults"
append_template "sdkconfig.region_$(printf '%s' "${PROFILE_REGION}" | tr '[:upper:]' '[:lower:]').defaults"

{
    printf 'CONFIG_PARTITION_TABLE_CUSTOM=y\n'
    printf 'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"partitions.csv\"\n'
    printf 'CONFIG_PARTITION_TABLE_FILENAME=\"partitions.csv\"\n'
} >> "${GENERATED_DEFAULTS}"

echo "Generated profile: ${FIRMWARE_ID}"
echo "Generated defaults: ${SDKCONFIG_DEFAULTS_PATH}"
echo "Target: ${IDF_TARGET_NAME}"
printf '\n--- partitions.csv ---\n'
cat "${ROOT_DIR}/partitions.csv"
printf '\n--- sdkconfig.defaults PARTITION_TABLE/FLASHSIZE ---\n'
grep -E 'CONFIG_(PARTITION_TABLE|ESPTOOLPY_FLASHSIZE)' "${SDKCONFIG_DEFAULTS_PATH}" || true

idf.py \
    -B "${BUILD_DIR}" \
    -D SDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS_PATH}" \
    set-target "${IDF_TARGET_NAME}"

idf.py \
    -B "${BUILD_DIR}" \
    -D SDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS_PATH}" \
    reconfigure

echo "=== effective sdkconfig partition config ==="
grep -E "CONFIG_PARTITION_TABLE|CONFIG_ESPTOOLPY_FLASHSIZE" "${BUILD_DIR}/sdkconfig" || true

idf.py \
    -B "${BUILD_DIR}" \
    -D SDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS_PATH}" \
    build

cp "${BUILD_DIR}/bootloader/bootloader.bin" "${FIRMWARE_DIR}/"
cp "${BUILD_DIR}/partition_table/partition-table.bin" "${FIRMWARE_DIR}/"
if [ -f "${BUILD_DIR}/ota_data_initial.bin" ]; then
    cp "${BUILD_DIR}/ota_data_initial.bin" "${FIRMWARE_DIR}/"
fi
cp "${BUILD_DIR}/esp32-rtk-gateway.bin" "${FIRMWARE_DIR}/"
if [ -f "${BUILD_DIR}/www.bin" ]; then
    cp "${BUILD_DIR}/www.bin" "${FIRMWARE_DIR}/"
fi

python3 - "${ROOT_DIR}" "${FIRMWARE_ID}" "${IDF_TARGET_NAME}" "${ROOT_DIR}/${PARTITION_SOURCE}" "${MANIFEST_PATH}" "${FIRMWARE_DIR}" <<'PY'
import csv
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
firmware_id = sys.argv[2]
idf_target = sys.argv[3]
partitions_path = pathlib.Path(sys.argv[4])
manifest_path = pathlib.Path(sys.argv[5])
firmware_dir = pathlib.Path(sys.argv[6])

chip_family = {
    "esp32": "ESP32",
    "esp32s3": "ESP32-S3",
    "esp32c3": "ESP32-C3",
    "esp32c6": "ESP32-C6",
}[idf_target]

def parse_size(value: str) -> int:
    text = value.strip().lower()
    if text.startswith("0x"):
        return int(text, 16)
    if text.endswith("k"):
        return int(float(text[:-1]) * 1024)
    if text.endswith("m"):
        return int(float(text[:-1]) * 1024 * 1024)
    return int(text, 10)

entries = []
next_offset = 0
for raw_line in partitions_path.read_text(encoding="utf-8").splitlines():
    line = raw_line.strip()
    if not line or line.startswith("#"):
        continue
    row = [column.strip() for column in next(csv.reader([line]))]
    while len(row) < 6:
        row.append("")
    name, part_type, subtype, offset_text, size_text, _flags = row[:6]
    size = parse_size(size_text)
    offset = parse_size(offset_text) if offset_text else next_offset
    entries.append({
        "name": name,
        "type": part_type,
        "subtype": subtype,
        "offset": offset,
        "size": size,
    })
    next_offset = offset + size

offsets = {entry["name"]: entry["offset"] for entry in entries}

parts = [
    {"path": f"../firmware/{firmware_id}/bootloader.bin", "offset": 0},
    {"path": f"../firmware/{firmware_id}/partition-table.bin", "offset": 0x8000},
]

ota_path = firmware_dir / "ota_data_initial.bin"
if ota_path.exists():
    parts.append({"path": f"../firmware/{firmware_id}/ota_data_initial.bin", "offset": offsets.get("otadata", 0xE000)})

parts.append({"path": f"../firmware/{firmware_id}/esp32-rtk-gateway.bin", "offset": offsets["ota_0"]})

www_path = firmware_dir / "www.bin"
if www_path.exists() and "www" in offsets:
    parts.append({"path": f"../firmware/{firmware_id}/www.bin", "offset": offsets["www"]})

manifest = {
    "name": f"ESP32 RTK Gateway - {firmware_id}",
    "version": firmware_id,
    "new_install_prompt_erase": True,
    "builds": [{
        "chipFamily": chip_family,
        "parts": parts,
    }],
}

manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
PY

printf '%s\n' "${FIRMWARE_ID}" > "${GENERATED_DIR}/firmware-id.txt"
printf '%s\n' "${FIRMWARE_DIR}" > "${GENERATED_DIR}/firmware-dir.txt"
printf '%s\n' "${MANIFEST_PATH}" > "${GENERATED_DIR}/manifest-path.txt"

echo "Firmware output: ${FIRMWARE_DIR}"
echo "Manifest output: ${MANIFEST_PATH}"
