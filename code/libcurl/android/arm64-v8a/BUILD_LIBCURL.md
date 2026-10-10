# Building libcurl.so for Android (arm64-v8a)

The bundled `libcurl.so` reports `libcurl/8.11.0` and has SHA-256
`da39bec6ed0c0e33812dda3848042b478d3a8eab870bdd2eb8e7d50074505c89`. The engine loads it with `dlopen`
and compiles against the headers in `code/libcurl/windows/include`.

The pre-built `libcurl.so` in this directory was built with SSL support using mbedTLS.
This document explains how to rebuild it if needed.

## Prerequisites

- Android NDK (tested with 21.1.6352462)
- CMake 3.10+
- Ninja (optional, but recommended)

## Source Downloads

Download and extract to a build directory:

1. **curl 8.11.0**: https://curl.se/download/curl-8.11.0.tar.gz
2. **mbedTLS 3.6.2**: https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2

## Build Script (Windows batch file)

Create `build-android-ssl.bat`:

```batch
@echo off
setlocal enabledelayedexpansion

REM Configuration
set ANDROID_API=26
set ABI=arm64-v8a
set BUILD_DIR=%~dp0build-%ABI%
set OUTPUT_DIR=%~dp0output-%ABI%
set CURL_SRC=%~dp0curl-8.11.0
set MBEDTLS_SRC=%~dp0mbedtls-3.6.2

REM Find NDK (adjust path as needed)
set NDK=%LOCALAPPDATA%\Android\Sdk\ndk\21.1.6352462

REM Toolchain file
set TOOLCHAIN_FILE=%NDK%\build\cmake\android.toolchain.cmake

echo Building mbedTLS...
mkdir "%BUILD_DIR%\mbedtls" 2>nul
cd /d "%BUILD_DIR%\mbedtls"

cmake -G "Ninja" ^
    -DCMAKE_TOOLCHAIN_FILE="%TOOLCHAIN_FILE%" ^
    -DANDROID_ABI=%ABI% ^
    -DANDROID_PLATFORM=android-%ANDROID_API% ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_INSTALL_PREFIX="%OUTPUT_DIR%" ^
    -DENABLE_PROGRAMS=OFF ^
    -DENABLE_TESTING=OFF ^
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF ^
    -DUSE_STATIC_MBEDTLS_LIBRARY=ON ^
    "%MBEDTLS_SRC%"

cmake --build . --config Release --parallel
cmake --install .

echo Building curl with mbedTLS...
mkdir "%BUILD_DIR%\curl" 2>nul
cd /d "%BUILD_DIR%\curl"

cmake -G "Ninja" ^
    -DCMAKE_TOOLCHAIN_FILE="%TOOLCHAIN_FILE%" ^
    -DANDROID_ABI=%ABI% ^
    -DANDROID_PLATFORM=android-%ANDROID_API% ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_INSTALL_PREFIX="%OUTPUT_DIR%" ^
    -DCMAKE_PREFIX_PATH="%OUTPUT_DIR%" ^
    -DBUILD_SHARED_LIBS=ON ^
    -DBUILD_CURL_EXE=OFF ^
    -DCURL_USE_OPENSSL=OFF ^
    -DCURL_USE_MBEDTLS=ON ^
    -DMBEDTLS_INCLUDE_DIRS="%OUTPUT_DIR%\include" ^
    -DMBEDTLS_LIBRARY="%OUTPUT_DIR%\lib\libmbedtls.a" ^
    -DMBEDX509_LIBRARY="%OUTPUT_DIR%\lib\libmbedx509.a" ^
    -DMBEDCRYPTO_LIBRARY="%OUTPUT_DIR%\lib\libmbedcrypto.a" ^
    -DCURL_USE_LIBSSH2=OFF ^
    -DCURL_USE_LIBPSL=OFF ^
    -DCURL_DISABLE_LDAP=ON ^
    -DCURL_DISABLE_LDAPS=ON ^
    -DUSE_LIBIDN2=OFF ^
    -DENABLE_MANUAL=OFF ^
    -DBUILD_TESTING=OFF ^
    -DCURL_CA_BUNDLE=none ^
    -DCURL_CA_PATH=none ^
    "%CURL_SRC%"

cmake --build . --config Release --parallel
cmake --install .

echo.
echo Build complete!
echo Output: %OUTPUT_DIR%\lib\libcurl.so
```

## Build Script (Linux/macOS)

```bash
#!/bin/bash
set -e

ANDROID_API=26
ABI=arm64-v8a
BUILD_DIR="$(pwd)/build-$ABI"
OUTPUT_DIR="$(pwd)/output-$ABI"
CURL_SRC="$(pwd)/curl-8.11.0"
MBEDTLS_SRC="$(pwd)/mbedtls-3.6.2"

# Find NDK
NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/21.1.6352462}"
TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"

echo "Building mbedTLS..."
mkdir -p "$BUILD_DIR/mbedtls"
cd "$BUILD_DIR/mbedtls"

cmake \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DANDROID_ABI=$ABI \
    -DANDROID_PLATFORM=android-$ANDROID_API \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$OUTPUT_DIR" \
    -DENABLE_PROGRAMS=OFF \
    -DENABLE_TESTING=OFF \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
    -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
    "$MBEDTLS_SRC"

cmake --build . --config Release --parallel
cmake --install .

echo "Building curl with mbedTLS..."
mkdir -p "$BUILD_DIR/curl"
cd "$BUILD_DIR/curl"

cmake \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DANDROID_ABI=$ABI \
    -DANDROID_PLATFORM=android-$ANDROID_API \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$OUTPUT_DIR" \
    -DCMAKE_PREFIX_PATH="$OUTPUT_DIR" \
    -DBUILD_SHARED_LIBS=ON \
    -DBUILD_CURL_EXE=OFF \
    -DCURL_USE_OPENSSL=OFF \
    -DCURL_USE_MBEDTLS=ON \
    -DMBEDTLS_INCLUDE_DIRS="$OUTPUT_DIR/include" \
    -DMBEDTLS_LIBRARY="$OUTPUT_DIR/lib/libmbedtls.a" \
    -DMBEDX509_LIBRARY="$OUTPUT_DIR/lib/libmbedx509.a" \
    -DMBEDCRYPTO_LIBRARY="$OUTPUT_DIR/lib/libmbedcrypto.a" \
    -DCURL_USE_LIBSSH2=OFF \
    -DCURL_USE_LIBPSL=OFF \
    -DCURL_DISABLE_LDAP=ON \
    -DCURL_DISABLE_LDAPS=ON \
    -DUSE_LIBIDN2=OFF \
    -DENABLE_MANUAL=OFF \
    -DBUILD_TESTING=OFF \
    -DCURL_CA_BUNDLE=none \
    -DCURL_CA_PATH=none \
    "$CURL_SRC"

cmake --build . --config Release --parallel
cmake --install .

echo ""
echo "Build complete!"
echo "Output: $OUTPUT_DIR/lib/libcurl.so"
```

## Output

After building, copy `output-arm64-v8a/lib/libcurl.so` to this directory.

## Licenses

- **curl**: MIT/X derivate license (see COPYING in the curl 8.11.0 source)
- **mbedTLS**: Apache License 2.0 (https://github.com/Mbed-TLS/mbedtls/blob/development/LICENSE)
