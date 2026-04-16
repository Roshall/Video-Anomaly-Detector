#!/usr/bin/env bash
# pack.sh  – Bundle the installed Analyzer outputs into a self-extracting
#             install.sh
#
# Usage:
#   ./pack.sh [OPTIONS] [STAGING_DIR]
#
#   STAGING_DIR  Directory produced by:
#                    cmake --install <build-dir> --prefix <STAGING_DIR>
#                Defaults to "./staging".
#
# The generated install.sh can be executed on the target machine:
#   sudo ./install.sh [INSTALL_PREFIX]       # default: /opt/VideoAnalyzer

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── Defaults ───────────────────────────────────────────────────────────────
# Use aarch64 cross-strip by default; --native switches to host strip.
STRIP_CMD="aarch64-linux-gnu-strip"
STAGING_DIR="${SCRIPT_DIR}/staging"
# Output filename for the generated installer script.
OUTPUT="${SCRIPT_DIR}/install.sh"

# Each component can be toggled independently.
# Defaults reproduce the original (all-inclusive, strip enabled) behaviour.
INCLUDE_BIN=1
INCLUDE_LIB=1
INCLUDE_RESOURCES=1   # alarm.wav + font
INCLUDE_ALSA_CONF=1   # ALSA configuration (share/alsa)
INCLUDE_MODELS=1
INCLUDE_TESTS=1
INCLUDE_CONFIG_INI=1  # MediaServer config file
DO_STRIP=1

# ── Usage ──────────────────────────────────────────────────────────────────
usage() {
    cat <<'USAGE'
Usage: ./pack.sh [OPTIONS] [STAGING_DIR]

Options:
  --native              Use host strip instead of aarch64-linux-gnu-strip
  --out <file>          Write output installer script to <file>
  --bin   / --no-bin    Include/exclude Analyzer binary         (default: include)
  --lib   / --no-lib    Include/exclude shared libraries        (default: include)
  --resources / --no-resources
                        Include/exclude alarm.wav + fonts       (default: include)
  --alsa-conf / --no-alsa-conf
                        Include/exclude ALSA configuration      (default: include)
  --models / --no-models
                        Include/exclude DL model files          (default: include)
  --tests / --no-tests  Include/exclude test scripts            (default: include)
  --config-ini / --no-config-ini
                        Include/exclude MediaServer config.ini  (default: include)
  --strip / --no-strip  Strip binaries/libraries                (default: strip)
  -h, --help            Show this help and exit

  STAGING_DIR           cmake --install prefix  (default: ./staging)
USAGE
}

# ── Argument parsing ───────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "${1}" in
        --native)       STRIP_CMD="strip"      ;;
        --out)
            if [[ -z "${2:-}" || "${2:-}" == --* ]]; then
                echo "ERROR: --out requires a file path" >&2
                usage >&2
                exit 1
            fi
            OUTPUT="${2}"
            shift
            ;;
        --bin)          INCLUDE_BIN=1          ;;
        --no-bin)       INCLUDE_BIN=0          ;;
        --lib)          INCLUDE_LIB=1          ;;
        --no-lib)       INCLUDE_LIB=0          ;;
        --resources)    INCLUDE_RESOURCES=1    ;;
        --no-resources) INCLUDE_RESOURCES=0    ;;
        --alsa-conf)    INCLUDE_ALSA_CONF=1    ;;
        --no-alsa-conf) INCLUDE_ALSA_CONF=0    ;;
        --models)       INCLUDE_MODELS=1       ;;
        --no-models)    INCLUDE_MODELS=0       ;;
        --tests)        INCLUDE_TESTS=1        ;;
        --no-tests)     INCLUDE_TESTS=0        ;;
        --config-ini)   INCLUDE_CONFIG_INI=1   ;;
        --no-config-ini) INCLUDE_CONFIG_INI=0  ;;
        --strip)        DO_STRIP=1             ;;
        --no-strip)     DO_STRIP=0             ;;
        --help|-h)      usage; exit 0          ;;
        -*)             echo "ERROR: Unknown option '${1}'" >&2; usage >&2; exit 1 ;;
        *)              STAGING_DIR="${1}"     ;;
    esac
    shift
done

# ── Locate installed files ─────────────────────────────────────────────────
BIN_DIR="${STAGING_DIR}/bin"
CONF_DIR="${STAGING_DIR}/conf"
CONFIG_FILE="${CONF_DIR}/config.json"
CONFIG_INI_FILE="${CONF_DIR}/config.ini"
TEST_API_PY="${SCRIPT_DIR}/test_api.py"
TEST_PARAMS_JSON="${SCRIPT_DIR}/test_params.json"

# config.json is always required (carries asset paths and runtime config).
if [[ ! -f "${CONFIG_FILE}" ]]; then
    echo "ERROR: config.json not found: ${CONFIG_FILE}" >&2
    echo "       Run: cmake --install <build-dir> --prefix '${STAGING_DIR}'" >&2
    exit 1
fi

if (( INCLUDE_BIN )); then
    if [[ ! -d "${BIN_DIR}" ]] || [[ -z "$(ls -A "${BIN_DIR}" 2>/dev/null)" ]]; then
        echo "ERROR: bin/ directory not found or empty: ${BIN_DIR}" >&2
        echo "       Run: cmake --install <build-dir> --prefix '${STAGING_DIR}'" >&2
        exit 1
    fi
fi

if (( INCLUDE_TESTS )); then
    if [[ ! -f "${TEST_API_PY}" ]]; then
        echo "ERROR: test_api.py not found: ${TEST_API_PY}" >&2
        exit 1
    fi
    if [[ ! -f "${TEST_PARAMS_JSON}" ]]; then
        echo "ERROR: test_params.json not found: ${TEST_PARAMS_JSON}" >&2
        exit 1
    fi
fi

if (( INCLUDE_CONFIG_INI )); then
    if [[ ! -f "${CONFIG_INI_FILE}" ]]; then
        echo "WARNING: config.ini not found: ${CONFIG_INI_FILE}" >&2
        echo "         MediaServer will not include config file." >&2
        INCLUDE_CONFIG_INI=0
    fi
fi

# ── Read asset paths from config.json ─────────────────────────────────────
MODELS_DIR=""
ALARM_WAV=""
FONT_FILE=""
FONT_BASENAME=""

if (( INCLUDE_MODELS || INCLUDE_RESOURCES )); then
    echo "Reading asset paths from config.json..."

    if (( INCLUDE_MODELS )); then
        MODELS_DIR="$(python3 -c \
            "import json,sys; print(json.load(open(sys.argv[1]))['DLModel']['dir'])" \
            "${CONFIG_FILE}")"
        echo "  DLModel.dir      : ${MODELS_DIR}"
    fi

    if (( INCLUDE_RESOURCES )); then
        ALARM_WAV="$(python3 -c \
            "import json,sys; print(json.load(open(sys.argv[1]))['alarm']['soundPath'])" \
            "${CONFIG_FILE}")"
        FONT_FILE="$(python3 -c \
            "import json,sys; print(json.load(open(sys.argv[1]))['display']['fontPath'])" \
            "${CONFIG_FILE}")"
        FONT_BASENAME="$(basename "${FONT_FILE}")"
        echo "  alarm.soundPath  : ${ALARM_WAV}"
        echo "  display.fontPath : ${FONT_FILE}"
    fi
fi

# ── Validate asset sources ─────────────────────────────────────────────────
errors=0
if (( INCLUDE_MODELS )); then
    [[ -d "${MODELS_DIR}" ]] \
        || { echo "ERROR: Models dir not found: ${MODELS_DIR}" >&2; errors=$((errors+1)); }
fi
if (( INCLUDE_RESOURCES )); then
    [[ -f "${ALARM_WAV}" ]] \
        || { echo "ERROR: Alarm WAV not found : ${ALARM_WAV}"  >&2; errors=$((errors+1)); }
    [[ -f "${FONT_FILE}" ]] \
        || { echo "ERROR: Font file not found : ${FONT_FILE}"  >&2; errors=$((errors+1)); }
fi
(( errors == 0 )) || exit 1

# ── Assemble payload in a temp workspace ──────────────────────────────────
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

PAYLOAD="${TMP_DIR}/payload"

# conf is always included (config.json drives the runtime).
mkdir -p "${PAYLOAD}/conf"
(( INCLUDE_BIN ))       && mkdir -p "${PAYLOAD}/bin"
(( INCLUDE_LIB ))       && mkdir -p "${PAYLOAD}/lib"
(( INCLUDE_ALSA_CONF )) && mkdir -p "${PAYLOAD}/share/alsa"
(( INCLUDE_MODELS ))    && mkdir -p "${PAYLOAD}/models"
(( INCLUDE_RESOURCES )) && mkdir -p "${PAYLOAD}/resources" "${PAYLOAD}/fonts"
(( INCLUDE_TESTS ))     && mkdir -p "${PAYLOAD}/tests"
(( INCLUDE_CONFIG_INI )) && mkdir -p "${PAYLOAD}/conf"

# ── Copy installed files ───────────────────────────────────────────────────
echo "Copying conf files..."
cp -a "${CONF_DIR}/." "${PAYLOAD}/conf/"

if (( INCLUDE_BIN )); then
    echo "Copying installed binaries..."
    cp -a "${BIN_DIR}/." "${PAYLOAD}/bin/"
fi

if (( INCLUDE_LIB )); then
    if [[ -d "${STAGING_DIR}/lib" ]]; then
        echo "Copying installed libraries..."
        cp -a "${STAGING_DIR}/lib/." "${PAYLOAD}/lib/"
    else
        echo "WARNING: ${STAGING_DIR}/lib not found – lib payload will be empty." >&2
    fi
fi

if (( INCLUDE_ALSA_CONF )); then
    if [[ -d "${STAGING_DIR}/share/alsa" ]]; then
        echo "Copying ALSA configuration..."
        cp -a "${STAGING_DIR}/share/alsa/." "${PAYLOAD}/share/alsa/"
    else
        echo "WARNING: ${STAGING_DIR}/share/alsa not found – ALSA config will not be included." >&2
    fi
fi

if (( INCLUDE_MODELS )); then
    echo "Copying models..."
    cp -r "${MODELS_DIR}/." "${PAYLOAD}/models/"
fi

if (( INCLUDE_RESOURCES )); then
    echo "Copying resources..."
    cp "${ALARM_WAV}" "${PAYLOAD}/resources/alarm.wav"
    echo "Copying font..."
    cp "${FONT_FILE}" "${PAYLOAD}/fonts/${FONT_BASENAME}"
fi

if (( INCLUDE_TESTS )); then
    echo "Copying test scripts..."
    cp "${TEST_API_PY}"       "${PAYLOAD}/tests/test_api.py"
    cp "${TEST_PARAMS_JSON}"  "${PAYLOAD}/tests/test_params.json"
    chmod +x "${PAYLOAD}/tests/test_api.py"
fi

if (( INCLUDE_CONFIG_INI )); then
    echo "Copying MediaServer config..."
    cp "${CONFIG_INI_FILE}" "${PAYLOAD}/conf/config.ini"
fi

# ── Strip binaries ─────────────────────────────────────────────────────────
if (( DO_STRIP )); then
    if (( INCLUDE_BIN )); then
        echo "Stripping binaries..."
        while IFS= read -r -d '' exe; do
            "${STRIP_CMD}" --strip-debug "${exe}" 2>/dev/null \
                && echo "  Stripped: bin/$(basename "${exe}")" || true
        done < <(find "${PAYLOAD}/bin" -maxdepth 1 -type f -print0)
    fi

    if (( INCLUDE_LIB )); then
        echo "Stripping shared libraries..."
        while IFS= read -r -d '' lib; do
            "${STRIP_CMD}" --strip-debug "${lib}" 2>/dev/null \
                && echo "  Stripped: lib/$(basename "${lib}")" || true
        done < <(find "${PAYLOAD}/lib" -maxdepth 2 -type f -name "*.so*" -print0)
    fi
else
    echo "Stripping skipped (--no-strip)."
fi

# ── Create compressed archive ──────────────────────────────────────────────
echo "Creating archive..."
ARCHIVE="${TMP_DIR}/payload.tar.gz"
tar -czf "${ARCHIVE}" -C "${PAYLOAD}" .
echo "  Archive size: $(du -sh "${ARCHIVE}" | cut -f1)"

# ── Generate install.sh ────────────────────────────────────────────────────
# The embedded installer detects what is present in the payload at runtime, so
# no pack-time conditionals are needed inside the installer body.  The only
# pack-time value wired in is FONT_BASENAME (needed to resolve the font path
# because the basename cannot be inferred from the payload structure alone).
echo "Generating install.sh..."

cat > "${OUTPUT}" << 'INSTALLER_HEADER'
#!/usr/bin/env bash
# install.sh  – Self-extracting installer for VideoAnalyzer
#
# Usage:  sudo ./install.sh [INSTALL_PREFIX] [OPTIONS]
#         Default INSTALL_PREFIX: /opt/VideoAnalyzer
#
# Options:
#   --storage-root <PATH>  Use alternate root for alarm_storage and offline_videos
#                          (default: under INSTALL_PREFIX)

set -euo pipefail

INSTALL_PREFIX="${1:-/opt/VideoAnalyzer}"
STORAGE_ROOT=""
FONT_BASENAME="__FONT_BASENAME__"
CONFIG_INI_INCLUDED=__CONFIG_INI_INCLUDED__

# exit if not run as root (required for writing to /opt and installing systemd services)
if [[ $EUID -ne 0 ]]; then
    echo "ERROR: This installer must be run as root (e.g. sudo ./install.sh [INSTALL_PREFIX] [OPTIONS])" >&2
    exit 1
fi

# ── Parse optional arguments ───────────────────────────────────────────────
while [[ $# -gt 1 ]]; do
    shift
    case "${1:-}" in
        --storage-root)
            if [[ -z "${2:-}" ]]; then
                echo "ERROR: --storage-root requires a path" >&2
                exit 1
            fi
            STORAGE_ROOT="${2}"
            shift
            ;;
        *)
            echo "ERROR: Unknown option '${1}'" >&2
            exit 1
            ;;
    esac
done

# Default storage root to INSTALL_PREFIX if not specified
if [[ -z "${STORAGE_ROOT}" ]]; then
    STORAGE_ROOT="${INSTALL_PREFIX}"
fi

# Normalize to an absolute path so config.json always contains absolute paths.
INSTALL_PREFIX="$(python3 -c 'import os,sys; print(os.path.abspath(sys.argv[1]))' "${INSTALL_PREFIX}")"

echo "Installing VideoAnalyzer to: ${INSTALL_PREFIX}"
[[ "${STORAGE_ROOT}" != "${INSTALL_PREFIX}" ]] && echo "Storage root: ${STORAGE_ROOT}"

# ── Create directory layout ────────────────────────────────────────────────
echo "Creating directories..."
mkdir -p \
    "${INSTALL_PREFIX}/bin"           \
    "${INSTALL_PREFIX}/lib"           \
    "${INSTALL_PREFIX}/conf"          \
    "${INSTALL_PREFIX}/share/alsa"    \
    "${INSTALL_PREFIX}/models"        \
    "${INSTALL_PREFIX}/resources"     \
    "${INSTALL_PREFIX}/fonts"         \
    "${INSTALL_PREFIX}/tests"         \
    "${STORAGE_ROOT}/alarm_storage"   \
    "${STORAGE_ROOT}/offline_videos"

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

# ── Install binaries ────────────────────────────────────────────────────────
if [[ -d "${_TMP}/bin" ]] && [[ -n "$(ls -A "${_TMP}/bin" 2>/dev/null)" ]]; then
    echo "Installing binaries..."
    find "${_TMP}/bin" -maxdepth 1 -type f | while read -r exe; do
        install -m 755 "${exe}" "${INSTALL_PREFIX}/bin/$(basename "${exe}")"
    done
fi

# ── Install libraries ──────────────────────────────────────────────────────
if [[ -d "${_TMP}/lib" ]] && [[ -n "$(ls -A "${_TMP}/lib" 2>/dev/null)" ]]; then
    echo "Installing libraries..."
    cp -a "${_TMP}/lib/." "${INSTALL_PREFIX}/lib/"
    find "${INSTALL_PREFIX}/lib" -maxdepth 2 -type f -name "*.so*" \
        -exec chmod 755 {} \;
fi

# ── Install models ─────────────────────────────────────────────────────────
if [[ -d "${_TMP}/models" ]] && [[ -n "$(ls -A "${_TMP}/models" 2>/dev/null)" ]]; then
    echo "Installing models..."
    cp -r "${_TMP}/models/." "${INSTALL_PREFIX}/models/"
    find "${INSTALL_PREFIX}/models" -type f -exec chmod 644 {} \;
fi

# ── Install resources ──────────────────────────────────────────────────────
if [[ -f "${_TMP}/resources/alarm.wav" ]]; then
    echo "Installing audio resource..."
    install -m 644 "${_TMP}/resources/alarm.wav" \
                   "${INSTALL_PREFIX}/resources/alarm.wav"
fi

if [[ -n "${FONT_BASENAME}" ]] && [[ -f "${_TMP}/fonts/${FONT_BASENAME}" ]]; then
    echo "Installing font..."
    install -m 644 "${_TMP}/fonts/${FONT_BASENAME}" \
                   "${INSTALL_PREFIX}/fonts/${FONT_BASENAME}"
fi

# ── Install ALSA configuration ──────────────────────────────────────────────
if [[ -d "${_TMP}/share/alsa" ]] && [[ -n "$(ls -A "${_TMP}/share/alsa" 2>/dev/null)" ]]; then
    echo "Installing ALSA configuration..."
    cp -a "${_TMP}/share/alsa/." "${INSTALL_PREFIX}/share/alsa/"
fi

# ── Install test scripts ────────────────────────────────────────────────────
if [[ -f "${_TMP}/tests/test_api.py" ]]; then
    echo "Installing test scripts..."
    install -m 755 "${_TMP}/tests/test_api.py" \
                   "${INSTALL_PREFIX}/tests/test_api.py"
    install -m 644 "${_TMP}/tests/test_params.json" \
                   "${INSTALL_PREFIX}/tests/test_params.json"
fi

# ── Install config.ini (if available) ──────────────────────────────────────
if (( CONFIG_INI_INCLUDED )); then
    if [[ -f "${_TMP}/conf/config.ini" ]]; then
        echo "Installing MediaServer config..."
        install -m 644 "${_TMP}/conf/config.ini" \
                       "${INSTALL_PREFIX}/conf/config.ini"
    fi
fi

# ── Install conf files ──────────────────────────────────────────────────────
if [[ -d "${_TMP}/conf" ]] && [[ -n "$(ls -A "${_TMP}/conf" 2>/dev/null)" ]]; then
    echo "Installing conf files..."
    cp -a "${_TMP}/conf/." "${INSTALL_PREFIX}/conf/"
fi

# ── Patch config.json paths to reflect the chosen install prefix ───────────
if [[ -f "${_TMP}/conf/config.json" ]]; then
    echo "Patching config.json..."
    python3 - "${_TMP}/conf/config.json" \
              "${INSTALL_PREFIX}/conf/config.json" \
              "${INSTALL_PREFIX}" \
              "${STORAGE_ROOT}" \
              "${FONT_BASENAME}" << 'PYEOF'
import json, os, sys

src, dst, prefix, storage_root, font_name = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
with open(src) as f:
    cfg = json.load(f)

# Only patch each path when the corresponding component was actually installed.
if os.path.isdir(f"{prefix}/models") and os.listdir(f"{prefix}/models"):
    cfg['DLModel']['dir'] = f"{prefix}/models"
if os.path.isdir(f"{storage_root}/alarm_storage"):
    cfg['uploadDir'] = f"{storage_root}/alarm_storage"
if os.path.isdir(f"{storage_root}/offline_videos"):
    cfg['offlineVideoDir'] = f"{storage_root}/offline_videos"
if os.path.isfile(f"{prefix}/resources/alarm.wav"):
    cfg['alarm']['soundPath'] = f"{prefix}/resources/alarm.wav"
if font_name and os.path.isfile(f"{prefix}/fonts/{font_name}"):
    cfg['display']['fontPath'] = f"{prefix}/fonts/{font_name}"

# Force runtime defaults for installed deployments.
cfg['acceleratorInference'] = False
cfg['logLevel'] = 'info'

with open(dst, 'w') as f:
    json.dump(cfg, f, indent=2, ensure_ascii=False)
    f.write('\n')
PYEOF
fi

# ── Set remaining permissions ──────────────────────────────────────────────
echo "Setting permissions..."
find "${INSTALL_PREFIX}/conf" -type f -exec chmod 644 {} \;
chmod 777 "${STORAGE_ROOT}/alarm_storage"
chmod 777 "${STORAGE_ROOT}/offline_videos"

# ── Generate systemd service files ─────────────────────────────────────────
echo "Creating systemd service files..."

# Create service files under the chosen install prefix, then copy to systemd
mkdir -p "${INSTALL_PREFIX}/service"

# Analyzer service
cat > "${INSTALL_PREFIX}/service/analyzer.service" << 'ANALYZER_SERVICE'
[Unit]
Description=Video Analyzer Service
After=network.target

[Service]
Type=simple
WorkingDirectory=__INSTALL_PREFIX__
Environment="ALSA_CONFIG_DIR=__INSTALL_PREFIX__/share/alsa"
Environment="ALSA_CONFIG_PATH=__INSTALL_PREFIX__/share/alsa/alsa.conf"
ExecStart=__INSTALL_PREFIX__/bin/Analyzer -f __INSTALL_PREFIX__/conf/config.json
Restart=on-failure
RestartSec=10s

[Install]
WantedBy=multi-user.target
ANALYZER_SERVICE

# MediaServer service
cat > "${INSTALL_PREFIX}/service/mediaserver.service" << 'MEDIASERVER_SERVICE'
[Unit]
Description=Media Server Service
After=network.target

[Service]
Type=simple
WorkingDirectory=__INSTALL_PREFIX__
ExecStart=__INSTALL_PREFIX__/bin/MediaServer -c __INSTALL_PREFIX__/conf/config.ini
Restart=on-failure
RestartSec=10s

[Install]
WantedBy=multi-user.target
MEDIASERVER_SERVICE

# Substitute install prefix in service files in the prefix dir
sed -i "s|__INSTALL_PREFIX__|${INSTALL_PREFIX}|g" "${INSTALL_PREFIX}/service/analyzer.service"
sed -i "s|__INSTALL_PREFIX__|${INSTALL_PREFIX}|g" "${INSTALL_PREFIX}/service/mediaserver.service"

# Copy service files into systemd unit dir so admin can enable/start them
cp "${INSTALL_PREFIX}/service/analyzer.service" /etc/systemd/system/
cp "${INSTALL_PREFIX}/service/mediaserver.service" /etc/systemd/system/

# Label the binary as an executable type
if [[ "${INSTALL_PREFIX}" == /home/* ]]; then
    chcon -t bin_t "${INSTALL_PREFIX}/bin/Analyzer"
    chcon -t bin_t "${INSTALL_PREFIX}/bin/MediaServer"
fi

# ── Done ───────────────────────────────────────────────────────────────────
echo ""
echo "======================================================"
echo " VideoAnalyzer installed successfully!"
echo " Prefix   : ${INSTALL_PREFIX}"
[[ -n "$(ls -A "${INSTALL_PREFIX}/bin" 2>/dev/null)" ]] \
                                               && echo " Binaries : ${INSTALL_PREFIX}/bin/"
[[ -n "$(ls -A "${INSTALL_PREFIX}/lib" 2>/dev/null)" ]] \
                                               && echo " Libs     : ${INSTALL_PREFIX}/lib/"
[[ -n "$(ls -A "${INSTALL_PREFIX}/share/alsa" 2>/dev/null)" ]] \
                                               && echo " ALSA     : ${INSTALL_PREFIX}/share/alsa/"
[[ -n "$(ls -A "${INSTALL_PREFIX}/models" 2>/dev/null)" ]] \
                                               && echo " Models   : ${INSTALL_PREFIX}/models/"
[[ -f "${INSTALL_PREFIX}/resources/alarm.wav" ]] \
                                               && echo " Resources: ${INSTALL_PREFIX}/resources/"
[[ -f "${INSTALL_PREFIX}/tests/test_api.py" ]] && echo " Tests    : ${INSTALL_PREFIX}/tests/"
echo " Config   : ${INSTALL_PREFIX}/conf/config.json"
echo " Storage  : ${STORAGE_ROOT}/alarm_storage/"
echo " Offline Videos  : ${STORAGE_ROOT}/offline_videos/"
echo ""

echo "Registering systemd services following installation..."
echo "systemctl daemon-reload"
echo "systemctl enable analyzer.service"
echo "systemctl enable mediaserver.service"

echo "Starting services following installation..."
echo "systemctl start analyzer.service"
echo "systemctl start mediaserver.service"
echo ""

echo " Systemd Services:"
echo "   - analyzer.service (Analyzer -f conf/config.json)"
echo "   - mediaserver.service (MediaServer -c conf/config.ini)"
echo ""
echo "======================================================"

exit 0
__ARCHIVE_BASE64__
INSTALLER_HEADER

# ── Substitute the font basename placeholder ───────────────────────────────
sed -i "s|__FONT_BASENAME__|${FONT_BASENAME}|g" "${OUTPUT}"

# ── Substitute config.ini inclusion flag ───────────────────────────────────
sed -i "s|__CONFIG_INI_INCLUDED__|${INCLUDE_CONFIG_INI}|g" "${OUTPUT}"

# ── Append base64-encoded payload archive ─────────────────────────────────
base64 "${ARCHIVE}" >> "${OUTPUT}"
chmod +x "${OUTPUT}"

echo ""
echo "Done!"
echo "  Output : ${OUTPUT}"
echo "  Size   : $(du -sh "${OUTPUT}" | cut -f1)"

