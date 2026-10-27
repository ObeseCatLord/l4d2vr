#!/usr/bin/env bash
# Builds Release/linux/l4d2vr_server.so (+ .vdf) for Linux i386 srcds.
#
#   L4D2VR/server/build_release_linux.sh            build inside docker (amd64 container)
#   L4D2VR/server/build_release_linux.sh --native   build with the local 32-bit g++
#   ... --tests                                     also run L4D2VR/server/run_server_tests.sh
#
# Output: ELF32 i386 shared object, libstdc++/libgcc linked statically,
# exporting only CreateInterface, needing only libc/libm (dynamic glibc,
# verified <= GLIBC_2.17 here; Marmot Cafe runs glibc 2.35).
#
# Container: L4D2VR_DOCKER_IMAGE (default debian:11-slim). The libstdc++.a and
# libgcc_eh.a of newer distributions reference newer glibc symbol versions
# (Debian 12: _dl_find_object@GLIBC_2.35, arc4random@2.36; Ubuntu 22.04:
# pthread_once@GLIBC_2.34), so a glibc-2.31 toolchain (Debian 11, GCC 10.2)
# is the default. Any Debian/Ubuntu amd64 image with apt works; the glibc check
# below rejects a build that needs more than GLIBC_2.17 unless
# L4D2VR_MAX_GLIBC is raised (never above 2.35 for Marmot Cafe).
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)
native=0
tests=0
for arg in "$@"; do
    case "$arg" in
        --native) native=1 ;;
        --tests) tests=1 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

if [[ $native -eq 0 ]]; then
    image=${L4D2VR_DOCKER_IMAGE:-debian:11-slim}
    inner="--native"
    [[ $tests -eq 1 ]] && inner="$inner --tests"
    exec docker run --rm --platform linux/amd64 \
        -v "$repo_root:/src" -w /src \
        -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
        -e L4D2VR_MAX_GLIBC="${L4D2VR_MAX_GLIBC:-2.17}" \
        "$image" bash -c "set -e
            export DEBIAN_FRONTEND=noninteractive
            apt-get update -qq >/dev/null
            apt-get install -y -qq --no-install-recommends g++-multilib binutils >/dev/null
            g++ --version | head -1
            ldd --version | head -1
            L4D2VR/server/build_release_linux.sh $inner
            chown -R \$HOST_UID:\$HOST_GID Release/linux"
fi

out_dir="$repo_root/Release/linux"
mkdir -p "$out_dir"
cxx=${CXX:-g++}
so="$out_dir/l4d2vr_server.so"
max_allowed=${L4D2VR_MAX_GLIBC:-2.17}

# -msse2 -mfpmath=sse: scalar SSE float math like server_srv.so and the MSVC
# client (no x87 excess precision in the usercmd codec).
# -fno-exceptions/-fno-rtti/-fno-threadsafe-statics + l4d2vr_libstdcxx_shims.h
# keep the C++ EH runtime (and its newer glibc symbols) out of the plugin.
"$cxx" -m32 -msse2 -mfpmath=sse -std=c++17 -O2 -fPIC -fvisibility=hidden -fvisibility-inlines-hidden \
    -fno-strict-aliasing -fno-exceptions -fno-rtti -fno-threadsafe-statics \
    -Wall -Wextra -Wpedantic -Werror \
    -shared "$script_dir/l4d2vr_server_linux.cpp" \
    -static-libstdc++ -static-libgcc \
    -Wl,-z,relro,-z,now,-z,noexecstack,--exclude-libs,ALL,--no-undefined \
    -Wl,--version-script="$script_dir/l4d2vr_server_linux.map" \
    -o "$so"
cp -f "$script_dir/l4d2vr_server.vdf" "$out_dir/l4d2vr_server.vdf"

# ---- verification (outputs captured first: no SIGPIPE under pipefail) ------
fail() { echo "verification failed: $*" >&2; exit 1; }
header=$(readelf -h "$so")
grep -q "ELF32" <<<"$header" || fail "not ELF32"
grep -q "Intel 80386" <<<"$header" || fail "not i386"
dynsym=$(objdump -T "$so")
exports=$(awk '$2 ~ /g/ && $4 != "*UND*" {print $NF}' <<<"$dynsym" | sort -u | tr '\n' ' ')
[[ "$exports" == "CreateInterface " ]] || fail "unexpected exports: $exports"
needed=$(readelf -d "$so" | awk '/NEEDED/ {gsub(/[\[\]]/, "", $5); print $5}' | sort -u | tr '\n' ' ')
for lib in $needed; do
    case "$lib" in
        libc.so.6|libm.so.6|libdl.so.2|libpthread.so.0|ld-linux.so.2) ;;
        *) fail "unexpected dependency $lib" ;;
    esac
done
max_glibc=$(grep -o 'GLIBC_[0-9.]*' <<<"$dynsym" | sed 's/GLIBC_//' | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)
highest=$(printf '%s\n%s\n' "$max_glibc" "$max_allowed" | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)
[[ "$highest" == "$max_allowed" ]] || fail "requires GLIBC_$max_glibc (> $max_allowed)"
disasm=$(objdump -d --no-show-raw-insn "$so")
struct_returns=$(grep -c 'ret    \$0x4' <<<"$disasm" || true)
[[ "$struct_returns" -gt 0 ]] || fail "no struct-return (ret \$4) detours found"

echo "built $so"
echo "  exports: $exports"
echo "  needs:   $needed"
echo "  glibc:   max GLIBC_$max_glibc (limit $max_allowed)"
echo "  callee-pop struct returns: $struct_returns"
sha256sum "$so" "$out_dir/l4d2vr_server.vdf"

if [[ $tests -eq 1 ]]; then
    OUT_DIR="$out_dir/tests" "$script_dir/run_server_tests.sh"
fi
