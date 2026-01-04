#!/usr/bin/env bash
set -euo pipefail

DEST="${1:-third_party/tinyexpr}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEST_PATH="${ROOT_DIR}/${DEST}"
SRC_BASE_URL="https://raw.githubusercontent.com/codeplea/tinyexpr/master"

FILES=("tinyexpr.c" "tinyexpr.h")
CHECKSUMS=(
  "1d81271c9368b6ef676c6532f0d0c0d21a81104ac569f6832c61f8c717e781bb"
  "4d7a0c30f5ec653b3b962d9eee24484d3a5f85a27393754feee6e47014b651c0"
)

mkdir -p "${DEST_PATH}"

for INDEX in "${!FILES[@]}"; do
  FILE="${FILES[$INDEX]}"
  CHECKSUM="${CHECKSUMS[$INDEX]}"
  URL="${SRC_BASE_URL}/${FILE}"
  TARGET="${DEST_PATH}/${FILE}"
  echo "Downloading ${FILE}..."
  curl -fsSL "${URL}" -o "${TARGET}"
  SUM_ACTUAL="$(shasum -a 256 "${TARGET}" | awk '{print $1}')"
  if [[ "${SUM_ACTUAL}" != "${CHECKSUM}" ]]; then
    echo "Checksum mismatch for ${FILE}"
    exit 1
  fi
done

cat > "${DEST_PATH}/README.md" <<'EOF'
# tinyexpr snapshot

Source: https://github.com/codeplea/tinyexpr (tag 1.0.4)

Pulled via `tools/ffi/install_tinyexpr.sh`.
EOF

echo "tinyexpr installed in ${DEST_PATH}"
