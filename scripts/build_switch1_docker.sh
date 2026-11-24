#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
IMAGE_NAME="${IMAGE_NAME:-levelup-switch-devkita64:latest}"
BASE_IMAGE="${BASE_IMAGE:-devkitpro/devkita64:latest}"
BUILD_DIR="${BUILD_DIR:-build/switch}"
STRIP_RESOURCES_FONT="${STRIP_RESOURCES_FONT:-0}"
LEVELUP_ENABLE_WEBRTC="${LEVELUP_ENABLE_WEBRTC:-1}"
LEVELUP_SWITCH_DEPS_PREFIX="${LEVELUP_SWITCH_DEPS_PREFIX:-${REPO_ROOT}/build/deps/switch}"

cd "${REPO_ROOT}"

if ! docker image inspect "${IMAGE_NAME}" >/dev/null 2>&1; then
    echo ">>> Building cached Switch 1 toolchain image: ${IMAGE_NAME} (from ${BASE_IMAGE})"
    docker build -t "${IMAGE_NAME}" - <<EOF
FROM ${BASE_IMAGE}
RUN dkp-pacman --noconfirm -R switch-enet
EOF
fi

if [[ "${STRIP_RESOURCES_FONT}" == "1" && -d "${REPO_ROOT}/resources/font" ]]; then
    echo ">>> Removing ./resources/font in build workspace (STRIP_RESOURCES_FONT=1)"
    rm -rf "${REPO_ROOT}/resources/font"
fi

CMAKE_WEBRTC_FLAGS="-DLEVELUP_ENABLE_WEBRTC=OFF"
if [[ "${LEVELUP_ENABLE_WEBRTC}" == "1" ]]; then
    if [[ ! -f "${LEVELUP_SWITCH_DEPS_PREFIX}/lib/libdatachannel.a" ]]; then
        echo ">>> Missing libdatachannel.a at ${LEVELUP_SWITCH_DEPS_PREFIX}/lib; running build_switch1_deps.sh"
        bash "${SCRIPT_DIR}/build_switch1_deps.sh"
    fi
    CMAKE_WEBRTC_FLAGS="-DLEVELUP_ENABLE_WEBRTC=ON -DLEVELUP_SWITCH_DEPS_PREFIX=/workspace/build/deps/switch"
fi

echo ">>> Cross-compiling Switch 1 NRO in ${BUILD_DIR} using ${IMAGE_NAME} (LEVELUP_ENABLE_WEBRTC=${LEVELUP_ENABLE_WEBRTC})"
docker run --rm \
    -u "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -e DEVKITPRO=/opt/devkitpro \
    -v "${REPO_ROOT}:/workspace" \
    -w /workspace \
    "${IMAGE_NAME}" \
    bash -lc '
        set -euo pipefail
        git config --global --add safe.directory "*"
        cmake -B "'"${BUILD_DIR}"'" -DCMAKE_BUILD_TYPE=Release -DPLATFORM_SWITCH=ON -DUSE_DEKO3D=ON '"${CMAKE_WEBRTC_FLAGS}"' || true
        cmake -B "'"${BUILD_DIR}"'" -DCMAKE_BUILD_TYPE=Release -DPLATFORM_SWITCH=ON -DUSE_DEKO3D=ON '"${CMAKE_WEBRTC_FLAGS}"'
        make -C "'"${BUILD_DIR}"'" Moonlight.nro -j"$(nproc)"
    '

NRO_PATH="${REPO_ROOT}/${BUILD_DIR}/Moonlight.nro"
if [[ ! -f "${NRO_PATH}" ]]; then
    echo "ERROR: Expected NRO not found at ${NRO_PATH}" >&2
    exit 1
fi

MAGIC="$(python3 -c '
import sys
with open(sys.argv[1], "rb") as f:
    f.seek(0x10)
    print(f.read(4).decode("ascii", errors="replace"))
' "${NRO_PATH}")"

if [[ "${MAGIC}" != "NRO0" ]]; then
    echo "ERROR: Invalid NRO magic at 0x10: ${MAGIC}" >&2
    exit 1
fi

ls -lh "${NRO_PATH}"
echo ">>> Verified NRO0 magic header: ${NRO_PATH}"
