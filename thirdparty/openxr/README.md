# Khronos OpenXR loader and headers (OpenXR backend)

* Source: Khronos `OpenXR-SDK` tag `release-1.1.65`
  (`https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.65.tar.gz`,
  sha256 `27aa797baf680c5f836b6138e78e59e2f72d6becfef6d4587ed9a57756d6d3b3`).
* `include/openxr/openxr.h`, `openxr_platform.h`, `openxr_platform_defines.h`: copied unchanged.
* `bin/win32/openxr_loader.dll`: the 32-bit loader built from that source with VS 2022 BuildTools
  (v143), `cmake -A Win32 -DDYNAMIC_LOADER=ON -DBUILD_TESTS=OFF`, Release, static CRT (the loader's
  own default for a DLL). Imports only `KERNEL32.dll` and `ADVAPI32.dll`.
  sha256 `cd0901f1f157f31653931afc923816e847a7edbd187453f2fa355eef894e98f1`, 605,696 bytes.
  Build script: `testtools/xr-m2/build-openxr-loader.sh` (outside the repo).
* Licence: Apache-2.0 (`LICENSE`). The loader contains jsoncpp (MIT / public domain), see
  `L4D2VR/THIRD_PARTY_NOTICES.md`.

The release package ships `openxr_loader.dll` next to `d3d9.dll`. The mod loads it by full path
from its own folder at run time (no import), so a missing loader only means "OpenXR unavailable".
Proton ships no Windows OpenXR loader; on Windows the active runtime's 32-bit manifest
(`HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ActiveRuntime`) is used.
