# Steam Frame development builds

These scripts cross-compile the Steam Frame client (Linux arm64, `TRINITY_FRAME=1`)
in WSL Ubuntu 22.04 and install it on a Frame over ssh. They run inside WSL and
expect the sibling checkouts `../trinity` and, optionally, `../OpenXR-SDK`.
Release packages come from the `steam-frame` job in `.github/workflows/release.yml`
instead.

One-time setup, as root:

```sh
wsl -d Ubuntu-22.04 -u root bash misc/frame/wsl-setup.sh
```

Then restart WSL (`wsl --shutdown`) so `appendWindowsPath=false` takes effect;
otherwise CMake finds MSYS2 packages through the Windows `PATH`.

The Frame needs ssh enabled and Trinity installed by the Trinity Installer, which
creates the `~/devkit-game/Trinity` title with the retail paks. Put the
`steamos` password in an untracked `.env` at the engine root:

```sh
FRAME_PASS=...
# optional: FRAME_HOST (default frame), FRAME_USER (default steamos), FRAME_TITLE (default Trinity)
```

| Script | Does |
|---|---|
| `build.sh` | Copies the engine and mod working trees (uncommitted changes included) into `~/src`, checks out the pinned OpenXR-SDK, and builds `build/release-linux-aarch64` with the native fallback modules and the loader. Extra arguments go to `make`. |
| `deploy.sh` | Copies the client, renderers, loader and native modules into the title, plus `pak8t.pk3`/`pak3t.pk3` from `../trinity/dist` when they differ. `-n` is a dry run. Refuses while Trinity runs. |
| `ssh.sh` | Runs a command on the Frame. |
| `sync-addon-paks.sh` | Sends a PC install's addon paks into the Frame's `~/.trinity`, skipping what is already there. |

Typical loop, from the engine root in Windows:

```sh
wsl -d Ubuntu-22.04 bash misc/frame/build.sh
wsl -d Ubuntu-22.04 bash misc/frame/deploy.sh
```

Build the mod's paks (`make` in `../trinity`) first when mod sources changed;
`build.sh` compiles the native modules from the mod sources, but `deploy.sh`
takes the paks from `../trinity/dist` as they are.

Always pass `ARCH=aarch64`, never `arm64`, if building by hand: `arm64` loses
the QVM JIT and names the renderers `trinity_*_arm64.so`, which the engine does
not load.
