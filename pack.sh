#!/usr/bin/env bash
# pack.sh  – Bundle the installed Analyzer outputs into a self-extracting
#             install.sh
#
# Usage:
#   ./pack.sh [STAGING_DIR]
#
#   STAGING_DIR  Directory produced by:
#                    cmake --install <build-dir> --prefix <STAGING_DIR>
#                Defaults to "./staging".
#
# The generated install.sh can be executed on the target machine:
#   sudo ./install.sh [INSTALL_PREFIX]       # default: /opt/VideoAnalyzer

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STAGING_DIR="${1:-${SCRIPT_DIR}/staging}"
OUTPUT="${SCRIPT_DIR}/install.sh"

# ── Locate installed files ─────────────────────────────────────────────────
BIN_FILE="${STAGING_DIR}/bin/Analyzer"
CONFIG_FILE="${STAGING_DIR}/conf/config.json"
TEST_API_PY="${SCRIPT_DIR}/test_api.py"
TEST_PARAMS_JSON="${SCRIPT_DIR}/test_params.json"

if [[ ! -f "${BIN_FILE}" ]]; then
    echo "ERROR: Analyzer binary not found: ${BIN_FILE}" >&2
    echo "       Run: cmake --install <build-dir> --prefix '${STAGING_DIR}'" >&2
    exit 1
fi
if [[ ! -f "${CONFIG_FILE}" ]]; then
    echo "ERROR: config.json not found: ${CONFIG_FILE}" >&2
    exit 1
fi
if [[ ! -f "${TEST_API_PY}" ]]; then
    echo "ERROR: test_api.py not found: ${TEST_API_PY}" >&2
    exit 1
fi
if [[ ! -f "${TEST_PARAMS_JSON}" ]]; then
    echo "ERROR: test_params.json not found: ${TEST_PARAMS_JSON}" >&2
    exit 1
fi

# ── Read asset paths from config.json ─────────────────────────────────────
echo "Reading asset paths from config.json..."

MODELS_DIR="$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['DLModel']['dir'])"           "${CONFIG_FILE}")"
ALARM_WAV="$(python3  -c "import json,sys; print(json.load(open(sys.argv[1]))['alarm']['soundPath'])"        "${CONFIG_FILE}")"
FONT_FILE="$(python3  -c "import json,sys; print(json.load(open(sys.argv[1]))['display']['fontPath'])"       "${CONFIG_FILE}")"

echo "  DLModel.dir      : ${MODELS_DIR}"
echo "  alarm.soundPath  : ${ALARM_WAV}"
echo "  display.fontPath : ${FONT_FILE}"

FONT_BASENAME="$(basename "${FONT_FILE}")"

# ── Validate asset sources ─────────────────────────────────────────────────
errors=0
[[ -d "${MODELS_DIR}" ]] || { echo "ERROR: Models dir not found: ${MODELS_DIR}" >&2; errors=$((errors+1)); }
[[ -f "${ALARM_WAV}"  ]] || { echo "ERROR: Alarm WAV not found : ${ALARM_WAV}"  >&2; errors=$((errors+1)); }
[[ -f "${FONT_FILE}"  ]] || { echo "ERROR: Font file not found : ${FONT_FILE}"  >&2; errors=$((errors+1)); }
(( errors == 0 )) || exit 1

# ── Assemble payload in a temp workspace ──────────────────────────────────
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

PAYLOAD="${TMP_DIR}/payload"
mkdir -p \
    "${PAYLOAD}/bin" \
    "${PAYLOAD}/lib" \
    "${PAYLOAD}/conf" \
    "${PAYLOAD}/models" \
    "${PAYLOAD}/resources" \
    "${PAYLOAD}/fonts" \
    "${PAYLOAD}/tests"

# ── Copy installed files ───────────────────────────────────────────────────
echo "Copying installed binary..."
cp -a "${BIN_FILE}" "${PAYLOAD}/bin/Analyzer"

echo "Copying installed libraries..."
if [[ -d "${STAGING_DIR}/lib" ]]; then
    cp -a "${STAGING_DIR}/lib/." "${PAYLOAD}/lib/"
fi

cp "${CONFIG_FILE}" "${PAYLOAD}/conf/config.json"

echo "Copying models..."
cp -r "${MODELS_DIR}/." "${PAYLOAD}/models/"

echo "Copying resources..."
cp "${ALARM_WAV}" "${PAYLOAD}/resources/alarm.wav"

echo "Copying font..."
cp "${FONT_FILE}" "${PAYLOAD}/fonts/${FONT_BASENAME}"

echo "Copying test scripts..."
cp "${TEST_API_PY}" "${PAYLOAD}/tests/test_api.py"
cp "${TEST_PARAMS_JSON}" "${PAYLOAD}/tests/test_params.json"
chmod +x "${PAYLOAD}/tests/test_api.py"

# ── Strip binaries ─────────────────────────────────────────────────────────
echo "Stripping binary..."
strip --strip-debug "${PAYLOAD}/bin/Analyzer" \
    && echo "  Stripped: bin/Analyzer"

echo "Stripping shared libraries..."
while IFS= read -r -d '' lib; do
    strip --strip-debug "${lib}" 2>/dev/null \
        && echo "  Stripped: lib/$(basename "${lib}")" || true
done < <(find "${PAYLOAD}/lib" -maxdepth 2 -type f -name "*.so*" -print0)

# ── Create compressed archive ──────────────────────────────────────────────
echo "Creating archive..."
ARCHIVE="${TMP_DIR}/payload.tar.gz"
tar -czf "${ARCHIVE}" -C "${PAYLOAD}" .
echo "  Archive size: $(du -sh "${ARCHIVE}" | cut -f1)"

# ── Generate install.sh header (single-quoted heredoc: no $-expansion) ────
# __FONT_BASENAME__ is a placeholder replaced by sed after the heredoc.
echo "Generating install.sh..."

cat > "${OUTPUT}" << 'INSTALLER_HEADER'
#!/usr/bin/env bash
# install.sh  – Self-extracting installer for VideoAnalyzer
#
# Usage:  sudo ./install.sh [INSTALL_PREFIX]
#         Default INSTALL_PREFIX: /opt/VideoAnalyzer

set -euo pipefail

INSTALL_PREFIX="${1:-/opt/VideoAnalyzer}"
FONT_BASENAME="__FONT_BASENAME__"

echo "Installing VideoAnalyzer to: ${INSTALL_PREFIX}"

# ── Create directory layout ────────────────────────────────────────────────
echo "Creating directories..."
mkdir -p \
    "${INSTALL_PREFIX}/bin" \
    "${INSTALL_PREFIX}/lib" \
    "${INSTALL_PREFIX}/conf" \
    "${INSTALL_PREFIX}/models" \
    "${INSTALL_PREFIX}/resources" \
    "${INSTALL_PREFIX}/fonts" \
    "${INSTALL_PREFIX}/tests" \
    "${INSTALL_PREFIX}/alarm_storage"

# ── Extract embedded payload ───────────────────────────────────────────────
echo "Extracting files..."
_TMP="$(mktemp -d)"
trap 'rm -rf "${_TMP}"' EXIT

_ARCHIVE_LINE=$(awk '/^__ARCHIVE_BASE64__$/{print NR+1; exit}' "$0")
if [[ -z "${_ARCHIVE_LINE}" ]]; then
    echo "ERROR: Payload marker not found in this script." >&2
    exit 1
fi
tail -n +"${_ARCHIVE_LINE}" "$0" | base64 -d | tar -xzf - -C "${_TMP}"

# ── Install binary ─────────────────────────────────────────────────────────
echo "Installing binary..."
install -m 755 "${_TMP}/bin/Analyzer" "${INSTALL_PREFIX}/bin/Analyzer"

# ── Install libraries ──────────────────────────────────────────────────────
echo "Installing libraries..."
if [[ -d "${_TMP}/lib" ]]; then
    cp -a "${_TMP}/lib/." "${INSTALL_PREFIX}/lib/"
    find "${INSTALL_PREFIX}/lib" -maxdepth 2 -type f -name "*.so*" \
        -exec chmod 755 {} \;
fi

# ── Install models ─────────────────────────────────────────────────────────
echo "Installing models..."
cp -r "${_TMP}/models/." "${INSTALL_PREFIX}/models/"
find "${INSTALL_PREFIX}/models" -type f -exec chmod 644 {} \;

# ── Install resources ──────────────────────────────────────────────────────
echo "Installing audio resource..."
install -m 644 "${_TMP}/resources/alarm.wav" \
               "${INSTALL_PREFIX}/resources/alarm.wav"

# ── Install font ───────────────────────────────────────────────────────────
echo "Installing font..."
install -m 644 "${_TMP}/fonts/${FONT_BASENAME}" \
               "${INSTALL_PREFIX}/fonts/${FONT_BASENAME}"

# ── Install test scripts ────────────────────────────────────────────────────
echo "Installing test scripts..."
install -m 755 "${_TMP}/tests/test_api.py" \
               "${INSTALL_PREFIX}/tests/test_api.py"
install -m 644 "${_TMP}/tests/test_params.json" \
               "${INSTALL_PREFIX}/tests/test_params.json"

# ── Patch config.json paths to reflect the chosen install prefix ───────────
echo "Patching config.json..."
python3 - "${_TMP}/conf/config.json" \
          "${INSTALL_PREFIX}/conf/config.json" \
          "${INSTALL_PREFIX}" \
          "${FONT_BASENAME}" << 'PYEOF'
import json, sys

src, dst, prefix, font_name = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
with open(src) as f:
    cfg = json.load(f)

cfg['DLModel']['dir']      = f"{prefix}/models"
cfg['uploadDir']           = f"{prefix}/alarm_storage"
cfg['alarm']['soundPath']  = f"{prefix}/resources/alarm.wav"
cfg['display']['fontPath'] = f"{prefix}/fonts/{font_name}"

with open(dst, 'w') as f:
    json.dump(cfg, f, indent=2, ensure_ascii=False)
    f.write('\n')
PYEOF

# ── Set remaining permissions ──────────────────────────────────────────────
echo "Setting permissions..."
chmod 644 "${INSTALL_PREFIX}/conf/config.json"
chmod 777 "${INSTALL_PREFIX}/alarm_storage"

# ── Done ───────────────────────────────────────────────────────────────────
echo ""
echo "======================================================"
echo " VideoAnalyzer installed successfully!"
echo " Prefix   : ${INSTALL_PREFIX}"
echo " Binary   : ${INSTALL_PREFIX}/bin/Analyzer"
echo " Config   : ${INSTALL_PREFIX}/conf/config.json"
echo " Models   : ${INSTALL_PREFIX}/models/"
echo " Tests    : ${INSTALL_PREFIX}/tests/"
echo " Storage  : ${INSTALL_PREFIX}/alarm_storage/"
echo "======================================================"

exit 0
__ARCHIVE_BASE64__
INSTALLER_HEADER

# ── Substitute the font basename placeholder ───────────────────────────────
sed -i "s|__FONT_BASENAME__|${FONT_BASENAME}|g" "${OUTPUT}"

# ── Append base64-encoded payload archive ─────────────────────────────────
base64 "${ARCHIVE}" >> "${OUTPUT}"
chmod +x "${OUTPUT}"

echo ""
echo "Done!"
echo "  Output : ${OUTPUT}"
echo "  Size   : $(du -sh "${OUTPUT}" | cut -f1)"
