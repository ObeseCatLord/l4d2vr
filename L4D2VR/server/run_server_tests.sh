#!/usr/bin/env bash
# Builds and runs the dedicated-server tests as 32-bit x86 (the plugin's ABI):
# the std-only server_core suites, the plugin callback harness and, when a
# server_srv.so path is given, the symbol-resolution self-test.
#
#   L4D2VR/server/run_server_tests.sh [path/to/server_srv.so]
#
# Needs a 32-bit capable g++ (g++-multilib); CXX overrides the compiler.
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)
out_dir=${OUT_DIR:-"$repo_root/Release/linux/tests"}
cxx=${CXX:-g++}
mkdir -p "$out_dir"

# SSE scalar math like the game binary and the MSVC client (no x87 excess precision).
flags=(-m32 -msse2 -mfpmath=sse -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror)

for suite in vr_server_usercmd_codec_tests vr_server_policy_tests vr_server_session_tests vr_server_hook_tests \
             vr_pose_protocol_v3_tests vr_remote_shell_tests vr_remote_magazine_tests vr_physical_controls_tests
do
    "$cxx" "${flags[@]}" "$repo_root/tests/$suite.cpp" -o "$out_dir/$suite"
    "$out_dir/$suite"
done

"$cxx" "${flags[@]}" -fvisibility=hidden "$script_dir/tests/l4d2vr_server_plugin_test.cpp" -o "$out_dir/l4d2vr_server_plugin_test"
"$out_dir/l4d2vr_server_plugin_test"

release_so="$repo_root/Release/linux/l4d2vr_server.so"
if [[ -f "$release_so" ]]; then
    "$cxx" "${flags[@]}" "$script_dir/tests/l4d2vr_server_dlopen_test.cpp" -ldl -o "$out_dir/l4d2vr_server_dlopen_test"
    "$out_dir/l4d2vr_server_dlopen_test" "$release_so"
fi

"$cxx" -std=c++17 -O1 -Wall -Wextra -Werror "$script_dir/tools/l4d2vr_server_symcheck.cpp" -o "$out_dir/l4d2vr_server_symcheck"
if [[ $# -ge 1 ]]; then
    "$out_dir/l4d2vr_server_symcheck" "$1"
fi
echo "server tests passed"
