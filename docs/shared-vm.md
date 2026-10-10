# Shared VM subsystem: upstream merge notes

The VM subsystem is upstream Quake3e's with the VR code kept beside it: `vm_vr.c`
(module selection: the pk3-priority QVM runs, a flat-only one gets the bundled
native modules where its rules allow; the VR API marker parser), `vm_vr_state.c`
(shared-state registration and sync) and `vm_vr.h`, which declares both.

**Merge upstream Quake3e here.** Deliberate divergences from upstream are tagged with
`[vm_vr]` comments, with these exceptions, unmarked but intentional:

- `vm_x86.c` spells the game module's inlined floor/ceil traps
  `~TRAP_FLOOR` / `~TRAP_CEIL` (upstream: `~G_FLOOR` / `~G_CEIL`). Same
  values (110/111), pinned by a compile-time assertion beside
  `sharedTraps_t` in `qcommon.h`.
- `vm_x86.c` carries this fork's pre-existing `#ifndef DEDICATED`
  guards.
- `vm.c`'s `VM_LoadQVM` is non-static (the policy files call it); the
  `[vm_vr]` marker sits on its declaration in `vm_local.h`, not at the
  definition.
