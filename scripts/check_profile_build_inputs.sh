#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${ROOT_DIR}"

legacy_files=()
while IFS= read -r path; do
    if [ -n "${path}" ] && [ -e "${path}" ]; then
        legacy_files+=("${path}")
    fi
done < <(git ls-files 'configs/sdkconfig.*' 'configs/skconfig.*')

if [ "${#legacy_files[@]}" -gt 0 ]; then
    echo "Legacy full sdkconfig files must not be tracked:" >&2
    printf ' - %s\n' "${legacy_files[@]}" >&2
    exit 1
fi

if rg -n 'configs/(s|sk)dkconfig\.' .github scripts configs/profiles --glob '!scripts/check_profile_build_inputs.sh' >/dev/null 2>&1; then
    echo "Found legacy full sdkconfig references in profile build inputs or CI." >&2
    rg -n 'configs/(s|sk)dkconfig\.' .github scripts configs/profiles --glob '!scripts/check_profile_build_inputs.sh' >&2
    exit 1
fi

echo "Profile build inputs are clean: templates-only sdkconfig flow enforced."
