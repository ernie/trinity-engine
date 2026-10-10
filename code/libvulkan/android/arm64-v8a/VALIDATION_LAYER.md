# libVkLayer_khronos_validation.so

Upstream binary, not built locally. Byte-identical to the `arm64-v8a` build in
Vulkan-ValidationLayers release `vulkan-sdk-1.4.335.0` (published 2025-12-09), confirmed by SHA-256
against the published archive.

```
source  https://github.com/KhronosGroup/Vulkan-ValidationLayers/releases/download/vulkan-sdk-1.4.335.0/android-binaries-1.4.335.0.zip
path    android-binaries-1.4.335.0/arm64-v8a/libVkLayer_khronos_validation.so
sha256  1b2d6bf67f60bd8194d0dd7c4283b695352766ba73e44e04974142713dcc082e
```

The binary carries no version string, so that checksum is the only thing tying this file to its
release. Re-derive it if the binary is ever replaced.

To update, take the `arm64-v8a` library from a newer release archive; `android/app/build.gradle`
packages this directory as native libraries.

## Why it ships

The layer is inert in normal play. It loads only when someone with adb enables Android's GPU debug
layers, which the release manifest permits via `com.android.graphics.injectLayers.enable`, so a
validation capture measures the binaries that actually ship.

## License

Apache License 2.0 — https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/main/LICENSE

Loaded by the Vulkan loader at runtime and not linked against the engine, so it is aggregated with
this GPL-2.0 application rather than combined with it. The license text ships to users in
`assets/THIRD-PARTY-NOTICES.txt`.
