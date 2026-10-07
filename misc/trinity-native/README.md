# Optional native VR modules

Normal builds leave `BUILD_TRINITY_NATIVE_FALLBACK` disabled. They do not look
for a sibling checkout or download mod sources. Enable it only with a Trinity
revision implementing the shared VR API 1.0.

Local CMake build:

```sh
cmake -S . -B build/local -DBUILD_TRINITY_NATIVE_FALLBACK=ON -DTRINITY_SOURCE_DIR=/path/to/trinity
cmake --build build/local
```

Local Make build (requires CMake as well as the normal engine toolchain):

```sh
make BUILD_TRINITY_NATIVE_FALLBACK=1 TRINITY_SOURCE_DIR=/path/to/trinity
```

For CI, omit `TRINITY_SOURCE_DIR` and supply `TRINITY_SOURCE_REVISION` containing
the full 40-digit Git commit. This explicitly enables fetching that pinned
commit from `https://github.com/ernie/trinity.git`. There is no default revision;
tags and branch names are rejected. A supplied local directory takes precedence.

The `trinity-native` target builds only the four native modules. With Make, `B`
selects the executable output directory (defaults to the release directory).
Debug/release builds keep separate CMake caches in their `native-build` folders.
`TRINITY_NATIVE_CMAKE_ARGS` passes additional CMake options. Cross builds other
than the same-platform x86 multilib case require
`TRINITY_NATIVE_TOOLCHAIN_FILE`; use `TRINITY_NATIVE_C_FLAGS` for extra compiler
architecture flags. On Windows, run in the full MSYS2 environment.

Output and packaging layout, relative to the client executable:

```text
baseq3/cgame<arch>.<extension>
baseq3/ui<arch>.<extension>
missionpack/cgame<arch>.<extension>
missionpack/ui<arch>.<extension>
```

Names use the engine's `ARCH_STRING`: Windows `x86`, `x86_64`, `arm32`,
`arm64` with `.dll`; Linux `i386`, `x86_64`, `arm`, `aarch64`, `ppc64`,
`ppc64le` with `.so`; modern macOS `x86_64` or `aarch64` with `.dylib`.
Universal macOS builds install the same universal library under both names.
No `lib` prefix is used. The modules live in the game directories beside the
client, next to that game's paks. CMake installation places the client and the
enabled modules at the installation root (inside `Contents/MacOS` of the app
bundle on macOS); Make installation copies `baseq3` and `missionpack` into
`DESTDIR`. Release packaging must keep the module files in those directories
beside the executable.
Use CMake's `--component TrinityNative` to stage only these four libraries.

The runtime refuses native fallback on pure servers. Enabling this build option
does not change that policy.

## Reproducible Windows/Linux releases

`misc/release-dependencies.json` pins the release inputs. GitHub Actions validates
those pins, checks the mod release's commit and asset hashes, and shares the
verified assets between packaging jobs. Windows/Linux jobs configure, build and
install the native modules and OpenXR loader through their CMake entry points.

Pin the published Trinity commit, matching release tag, and asset hashes before
release packaging. Missing pins fail the release gate; developer and dedicated
builds remain available.

`TRINITY_RELEASE_BUILD=ON` in CMake verifies the source checkout is clean and
matches the full pin, and checks VR state declarations against the engine. The
release version string is the pinned commit, independent of locally available
tags. Visual Studio uses the static CRT; ARM64 packaging uses `-A ARM64` for both
native modules and the loader. Linux armv7 uses the checked-in cross toolchain.

macOS VR is not enabled by this release packaging policy.

CI builds the pinned mod when a revision is declared. With an empty pin it
builds the mod's `main` branch. Release packaging still requires published,
immutable pins and never uses that fallback.
