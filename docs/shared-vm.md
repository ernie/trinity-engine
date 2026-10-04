# Shared VM subsystem: upstream merge notes

These files are byte-identical with trinity-standalone: `vm.c`, `vm_local.h`,
`vm_vr.h`, `vm_interpreted.c`, `vm_x86.c`, `vm_aarch64.c`, `vm_armv7l.c`,
`vm_optimize.h`, `vm_vr_state.c` (shared-state registration and sync),
`vm_vr_select.c` and `vm_vr_select.h` (the VR API marker parser).

Per-engine files sit beside them: `vm_vr.c` (module-selection policy: the
pk3-priority QVM runs, a flat-only one gets the engine's native modules where
its rules allow), `vm_vr_fallback.h` here (the bundled fallback API that
`cl_vr_modules.c` drives), and each side's own `vrcommon/vr_state.h`, which
declares the `VR_Shared*` sync functions `vm_vr_state.c` calls.

**Merge upstream Quake3e here first**, then copy the shared files outward to
trinity-standalone verbatim (verify with committed blob hashes, not
working-tree diffs). Deliberate divergences from upstream are tagged with
`[vm_vr]` comments, with these exceptions, unmarked but intentional:

- `vm_x86.c` spells the game module's inlined floor/ceil traps
  `~TRAP_FLOOR` / `~TRAP_CEIL` (upstream: `~G_FLOOR` / `~G_CEIL`). Same
  values (110/111), pinned by a compile-time assertion beside
  `sharedTraps_t` in `qcommon.h` in both engines.
- `vm_x86.c` carries this fork's pre-existing `#ifndef DEDICATED`
  guards.
- `vm.c`'s `VM_LoadQVM` is non-static (the policy files call it); the
  `[vm_vr]` marker sits on its declaration in `vm_local.h`, not at the
  definition.
