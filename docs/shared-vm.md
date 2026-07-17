# Shared VM subsystem — upstream merge notes

The VM subsystem is shared byte-identically with trinity-vr and
trinity-quest: `vm.c`, `vm_local.h`, `vm_vr.h`, `vm_interpreted.c`,
`vm_x86.c`, `vm_aarch64.c`, `vm_armv7l.c`, `vm_optimize.h`. The only
per-engine VM file is `vm_vr.c` (module-selection policy: stock Quake3e
semantics here, the VR QVM ladder in the VR engines).

**Merge upstream Quake3e here first**, then copy the eight shared files
outward to trinity-vr and trinity-quest verbatim (verify with committed
blob hashes, not working-tree diffs). Deliberate divergences from
upstream are tagged with `[vm_vr]` comments, with these exceptions —
unmarked but intentional:

- `vm_x86.c` spells the game module's inlined floor/ceil traps
  `~TRAP_FLOOR` / `~TRAP_CEIL` (upstream: `~G_FLOOR` / `~G_CEIL`). Same
  values (110/111), pinned by a compile-time assertion beside
  `sharedTraps_t` in `qcommon.h` in all three engines.
- `vm_x86.c` carries this fork's pre-existing `#ifndef DEDICATED`
  guards.
- `vm.c`'s `VM_LoadQVM` is non-static (the policy files call it); the
  `[vm_vr]` marker sits on its declaration in `vm_local.h`, not at the
  definition.
