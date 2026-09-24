# Optional OpenXR loader

The client discovers the loader at runtime; flatscreen and dedicated-server
operation do not require it. This helper builds Khronos OpenXR SDK 1.1.45 at
the revision in `misc/release-dependencies.json`, matching the engine headers.
JSON support is built in. MinGW compiler support is linked into the DLL.

Normal engine builds opt in with `BUILD_OPENXR_LOADER=1` (Make) or
`-DBUILD_OPENXR_LOADER=ON` (CMake). `make openxr-loader` builds only the loader.
`make install BUILD_OPENXR_LOADER=1` stages it and its licenses with the client.

From the platform development environment:

```sh
cmake -S misc/openxr-loader -B build/loader -DCMAKE_BUILD_TYPE=Release
cmake --build build/loader --target openxr_loader --parallel
cmake --install build/loader --component TrinityOpenXRLoader --prefix /path/to/client
```

For an offline build, pass
`-DTRINITY_OPENXR_SOURCE_DIR=/path/to/OpenXR-SDK` at configuration. Otherwise
the helper fetches the pinned official source. Match the loader architecture
to the client. Use a CMake toolchain file for cross compilation.

Windows packages place `openxr_loader.dll` beside `trinity.exe`, with the
installed license files. Linux and macOS first try the loader beside the
executable, then the system loader search path. Linux can instead use the
distribution's `libopenxr_loader.so.1`. Release packaging does not include a
macOS loader; a local macOS build must supply its own signed loader in
`Contents/MacOS` and validate the selected runtime.
A loader alone does not supply a headset runtime.

Release workflows build and install this loader through CMake using the immutable
revision in `misc/release-dependencies.json`. Installation includes both license
files. Local source overrides in release mode must be clean and exactly match
the declared commit.
