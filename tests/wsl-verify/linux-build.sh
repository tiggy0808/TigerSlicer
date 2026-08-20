#!/usr/bin/env bash
# Configure, build, and run TigerSlicer's test suites natively on Linux (WSL).
#
# Windows Smart App Control (enforced) rejects newly linked Windows test
# binaries on this machine, so the Catch2 suites cannot be started there. This
# script provides a full, real execution path for everything that does not need
# the Windows GUI stack: it builds libslic3r and the test targets from the same
# sources and runs the actual suites.
#
# One-time host prerequisites (installed with apt in the WSL distribution):
#   build-essential cmake ninja-build pkg-config libboost-all-dev libtbb-dev
#   libeigen3-dev libexpat1-dev libpng-dev zlib1g-dev libjpeg-dev libqhull-dev
#   libnlopt-dev libnlopt-cxx-dev libcgal-dev libgmp-dev libmpfr-dev
#   libcereal-dev nlohmann-json3-dev catch2 libcurl4-openssl-dev libopenvdb-dev
#   libblosc-dev libglew-dev libudev-dev libdbus-1-dev cargo rustc unzip
# plus LibBGCode built from the pinned revision into ${DEPS_PREFIX}.
#
# Usage (from Windows):  wsl -d Ubuntu -- bash tests/wsl-verify/linux-build.sh [targets...]
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${TS_LINUX_BUILD_DIR:-$HOME/tsbuild}"
DEPS_PREFIX="${TS_LINUX_DEPS_PREFIX:-$HOME/tsdeps/prefix}"
# Dependencies built at the exact revisions pinned under deps/ by the
# repository's own dependency builder. These must take precedence over the
# distribution packages: Ubuntu ships Boost 1.90 and CGAL 6, both of which are
# API-incompatible with these sources.
PINNED_PREFIX="${TS_LINUX_PINNED_PREFIX:-$HOME/tsdeps-full/usr/local}"
OPENVDB_MODULES="/usr/lib/x86_64-linux-gnu/cmake/OpenVDB"

export CMAKE_POLICY_VERSION_MINIMUM=3.5

mkdir -p "${BUILD_DIR}"

echo "== configuring (${BUILD_DIR})"
if ! cmake -S "${REPO}" -B "${BUILD_DIR}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DSLIC3R_GUI=0 \
        -DSLIC3R_BUILD_TESTS=1 \
        -DSLIC3R_STATIC=1 \
        -DSLIC3R_ENABLE_FORMAT_STEP=0 \
        -DOPENVDB_FIND_MODULE_PATH="${OPENVDB_MODULES}" \
        -DCMAKE_PREFIX_PATH="${PINNED_PREFIX};${DEPS_PREFIX}" \
        -DCMAKE_CXX_FLAGS="-I${PINNED_PREFIX}/include -I${DEPS_PREFIX}/include" \
        > "${BUILD_DIR}/configure.log" 2>&1; then
    echo "configure FAILED - last errors:"
    grep -E "CMake Error" -A6 "${BUILD_DIR}/configure.log" | head -40
    exit 1
fi
echo "   configure ok"

TARGETS=("$@")
if [[ ${#TARGETS[@]} -eq 0 ]]; then
    TARGETS=(libslic3r_tests fff_print_tests arrange_tests thumbnails_tests)
fi

echo "== building: ${TARGETS[*]}"
if ! cmake --build "${BUILD_DIR}" --target "${TARGETS[@]}" \
        > "${BUILD_DIR}/build.log" 2>&1; then
    echo "build FAILED - last errors:"
    grep -E "error:" "${BUILD_DIR}/build.log" | head -30
    exit 1
fi
echo "   build ok"

fail=0
for target in "${TARGETS[@]}"; do
    exe="$(find "${BUILD_DIR}" -type f -name "${target}" -perm -u+x | head -1)"
    if [[ -z "${exe}" ]]; then
        echo "== ${target}: executable not found"; fail=1; continue
    fi
    echo "== running ${target}"
    # Several suites resolve fixtures relative to their own directory.
    if (cd "$(dirname "${exe}")" && "${exe}" 2>&1 | tail -4); then
        :
    else
        fail=1
    fi
done

if [[ ${fail} -ne 0 ]]; then
    echo "LINUX TEST RUN: failures present"
    exit 1
fi
echo "LINUX TEST RUN: all suites reported success"
