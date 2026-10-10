SDL 2.32.10 (tag release-2.32.10, commit 5d249570393f7a37e037abf22cd6012a4cc56a71)

Licensed under the zlib license.

Source: https://github.com/libsdl-org/SDL/releases/tag/release-2.32.10
Archive: SDL2-devel-2.32.10-mingw.tar.gz
sha256 83a5d74012311edc3c0d40ea6faecbe57ad692aa033fa5dc273cc937e3938ff2

- include/SDL2: the archive's x86_64-w64-mingw32/include/SDL2. Its SDL_config.h
  picks the per-platform SDL_config_*.h, so MinGW and macOS each get their own.
- windows/mingw/lib64: the archive's x86_64 SDL2.dll renamed to SDL264.dll. The
  import library is generated so it names SDL264.dll (the output name gives the
  _head_SDL264_dll symbols):
    gendef - SDL2.dll > SDL264.def   (then LIBRARY "SDL264.dll")
    dlltool -m i386:x86-64 -d SDL264.def --dllname SDL264.dll -l SDL264.dll
    mv SDL264.dll libSDL264.dll.a
- windows/mingw/lib32: the archive's i686 SDL2.dll and libSDL2.dll.a as shipped.
- macosx: misc/macos/build-sdl.sh builds libSDL2-2.0.0.dylib, universal (arm64 + x86_64),
  from sdl_revision in misc/release-dependencies.json; the macOS CI and release jobs run it before make.
