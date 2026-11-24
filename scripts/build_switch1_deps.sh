#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
IMAGE_NAME="${IMAGE_NAME:-levelup-switch-devkita64:latest}"
BASE_IMAGE="${BASE_IMAGE:-devkitpro/devkita64:latest}"
DEPS_ROOT="${DEPS_ROOT:-${REPO_ROOT}/build/deps}"
SRC_ROOT="${DEPS_ROOT}/src"
WORK_ROOT="${DEPS_ROOT}/work"
PREFIX="${DEPS_ROOT}/switch"
CLEAN_DEPS="${CLEAN_DEPS:-0}"

MBEDTLS_GIT_URL="${MBEDTLS_GIT_URL:-https://github.com/Mbed-TLS/mbedtls.git}"
MBEDTLS_REF="${MBEDTLS_REF:-v3.6.2}"
LIBDATACHANNEL_GIT_URL="${LIBDATACHANNEL_GIT_URL:-https://github.com/paullouisageneau/libdatachannel.git}"
LIBDATACHANNEL_REF="${LIBDATACHANNEL_REF:-2afeedc70878043bcd59d2d726e1476824f687c3}"

cd "${REPO_ROOT}"

if ! docker image inspect "${IMAGE_NAME}" >/dev/null 2>&1; then
    echo ">>> Building cached Switch 1 toolchain image: ${IMAGE_NAME} (from ${BASE_IMAGE})"
    docker build -t "${IMAGE_NAME}" - <<EOF
FROM ${BASE_IMAGE}
RUN dkp-pacman --noconfirm -R switch-enet
EOF
fi

if [[ "${CLEAN_DEPS}" == "1" ]]; then
    echo ">>> Cleaning Switch 1 deps workspace: ${DEPS_ROOT}"
    rm -rf "${SRC_ROOT}/mbedtls" "${SRC_ROOT}/libdatachannel" "${WORK_ROOT}/mbedtls" "${WORK_ROOT}/libdatachannel" "${PREFIX}"
fi

mkdir -p "${SRC_ROOT}" "${WORK_ROOT}" "${PREFIX}"

if [[ ! -d "${SRC_ROOT}/mbedtls/.git" ]]; then
    echo ">>> Fetching MbedTLS ${MBEDTLS_REF}"
    git clone --depth 1 --branch "${MBEDTLS_REF}" "${MBEDTLS_GIT_URL}" "${SRC_ROOT}/mbedtls"
fi
if [[ ! -f "${SRC_ROOT}/mbedtls/framework/CMakeLists.txt" ]]; then
    echo ">>> Initializing MbedTLS framework submodule"
    git -C "${SRC_ROOT}/mbedtls" submodule update --init --depth 1
fi

if [[ ! -d "${SRC_ROOT}/libdatachannel/.git" ]]; then
    echo ">>> Fetching libdatachannel ${LIBDATACHANNEL_REF}"
    git clone "${LIBDATACHANNEL_GIT_URL}" "${SRC_ROOT}/libdatachannel"
    git -C "${SRC_ROOT}/libdatachannel" checkout "${LIBDATACHANNEL_REF}"
    git -C "${SRC_ROOT}/libdatachannel" submodule update --init --recursive \
        deps/libjuice deps/libsrtp deps/usrsctp deps/json deps/plog
fi

echo ">>> Applying Horizon OS (Switch 1) patches"
python3 "${REPO_ROOT}/scripts/switch1/patch_webrtc_deps.py" \
    --mbedtls-dir "${SRC_ROOT}/mbedtls" \
    --libdatachannel-dir "${SRC_ROOT}/libdatachannel"

echo ">>> Cross-compiling MbedTLS 3.6.2 and libdatachannel for aarch64-none-elf"
docker run --rm \
    -u "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -e DEVKITPRO=/opt/devkitpro \
    -v "${REPO_ROOT}:/workspace" \
    -w /workspace \
    "${IMAGE_NAME}" \
    bash -lc '
        set -euo pipefail
        NJOBS="$(nproc)"

        cmake -S /workspace/build/deps/src/mbedtls -B /workspace/build/deps/work/mbedtls \
            -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" \
            -DCMAKE_INSTALL_PREFIX=/workspace/build/deps/switch \
            -DMBEDTLS_FATAL_WARNINGS=OFF \
            -DENABLE_PROGRAMS=OFF \
            -DENABLE_TESTING=OFF \
            -DUSE_SHARED_MBEDTLS_LIBRARY=OFF
        cmake --build /workspace/build/deps/work/mbedtls --target install -j"${NJOBS}"

        cmake -S /workspace/build/deps/src/libdatachannel -B /workspace/build/deps/work/libdatachannel \
            -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" \
            -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" \
            -DCMAKE_INSTALL_PREFIX=/workspace/build/deps/switch \
            -DCMAKE_PREFIX_PATH=/workspace/build/deps/switch \
            -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON \
            -DMbedTLS_DIR=/workspace/build/deps/switch/lib/cmake/MbedTLS \
            -DUSE_MBEDTLS=ON \
            -DUSE_GNUTLS=OFF \
            -DUSE_NICE=OFF \
            -DNO_EXAMPLES=ON \
            -DNO_TESTS=ON \
            -DNO_MEDIA=OFF \
            -DNO_WEBSOCKET=OFF \
            -DNO_SERVER=ON \
            -DBUILD_SHARED_LIBS=OFF \
            -DBUILD_SHARED_DEPS_LIBS=OFF \
            -DINSTALL_DEPS_LIBS=ON \
            -DWARNINGS_AS_ERRORS=OFF \
            -DENABLE_WARNINGS_AS_ERRORS=OFF \
            -DSCTP_USE_MBEDTLS_SHA1=OFF
        cmake --build /workspace/build/deps/work/libdatachannel --target install -j"${NJOBS}"

        if [[ -f /workspace/build/deps/switch/lib/libjuice-static.a ]]; then
            cp -f /workspace/build/deps/switch/lib/libjuice-static.a /workspace/build/deps/switch/lib/libjuice.a
        elif [[ -f /workspace/build/deps/work/libdatachannel/deps/libjuice/libjuice.a ]]; then
            cp -f /workspace/build/deps/work/libdatachannel/deps/libjuice/libjuice.a /workspace/build/deps/switch/lib/libjuice.a
        fi
        if [[ -f /workspace/build/deps/work/libdatachannel/deps/libsrtp/libsrtp2.a ]]; then
            cp -f /workspace/build/deps/work/libdatachannel/deps/libsrtp/libsrtp2.a /workspace/build/deps/switch/lib/libsrtp2.a
        fi
        if [[ -f /workspace/build/deps/work/libdatachannel/deps/usrsctp/usrsctplib/libusrsctp.a ]]; then
            cp -f /workspace/build/deps/work/libdatachannel/deps/usrsctp/usrsctplib/libusrsctp.a /workspace/build/deps/switch/lib/libusrsctp.a
        fi

        mkdir -p /workspace/build/deps/switch/include/nlohmann
        cp -f /workspace/build/deps/src/libdatachannel/deps/json/single_include/nlohmann/json.hpp \
            /workspace/build/deps/switch/include/nlohmann/json.hpp

        echo ">>> Verifying AArch64 static archives"
        READELF=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-readelf
        NM=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm
        for lib in \
            libmbedtls.a libmbedx509.a libmbedcrypto.a libeverest.a libp256m.a \
            libdatachannel.a libjuice.a libsrtp2.a libusrsctp.a
        do
            path="/workspace/build/deps/switch/lib/${lib}"
            test -s "${path}"
            "${READELF}" -h "${path}" > /tmp/elf_hdr.txt
            grep -m1 "Machine:" /tmp/elf_hdr.txt | grep -q "AArch64"
        done
        rm -f /tmp/elf_hdr.txt

        "${NM}" -C /workspace/build/deps/switch/lib/libjuice.a > /tmp/juice_nm.txt
        grep -q "juice_create" /tmp/juice_nm.txt
        grep -q "udp_get_addrs" /tmp/juice_nm.txt
        grep -q "ice_create_local_candidate" /tmp/juice_nm.txt
        rm -f /tmp/juice_nm.txt

        strings /workspace/build/deps/switch/lib/libjuice.a > /tmp/juice_str.txt
        grep -q "SWITCH-ADDR-DIAG" /tmp/juice_str.txt
        grep -q "223.5.5.5" /tmp/juice_str.txt
        rm -f /tmp/juice_str.txt

        for rtclib in libdatachannel.a libjuice.a; do
            if "${NM}" -u "/workspace/build/deps/switch/lib/${rtclib}" | grep -qw "getnameinfo"; then
                echo "FAIL: ${rtclib} still has undefined reference to getnameinfo"
                exit 1
            fi
        done

        ls -lh /workspace/build/deps/switch/lib/*.a
    '

echo ">>> Switch 1 WebRTC deps ready at: ${PREFIX}"
