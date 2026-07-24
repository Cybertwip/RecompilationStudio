# Chrono Cross rumble-prompt compiled-call boundary

## Symptom

Selecting **On** at the `Vibration Function` prompt entered a tight overlay loop.
The runtime watchdog showed the interpreted overlay at `0x801D2CB0` repeatedly
calling static function `0x80013450`, then arriving at `0x801D2CB8` with `$v0=0`.

## Proof

- Ghidra/disassembly for `0x80013450` is a six-instruction accessor that loads
  the pointer at `0x80063EE8`, reads the pointed word, and returns it.
- Native TCP `read_ram` proved `0x80063EE8 = 0x1F801814` (GPUSTAT).
- Native TCP/rtrace proved GPUSTAT was nonzero (`0x564E021F` and
  `0x1482060A`). GPU hardware state and pointer initialization were not the zero
  source.
- The existing rumble-point watchdog capture records:
  `0x801D2CB0 -> jal 0x80013450`, followed by the interpreted continuation at
  `0x801D2CB8` with `$v0=0`.

Machine-readable evidence is in
`runtime/proofs/chrono_cross_gpustat_call_boundary.json`.

## Root cause and fix

`psx_dispatch_game_compiled()` directly invokes generated C. A normal generated
callee returns with `cpu->pc == guest_return_pc`. The dirty interpreter and
sljit call helpers checked `cpu->pc != 0` before consuming that normal return,
misclassifying it as a nonlocal transfer. They also failed to bracket static
compiled callees with the atomic call-unit depth used by native overlay callees.

The runtime now:

1. validates the call contract;
2. normalizes `cpu->pc == return_pc` to the dispatch convention `pc=0`;
3. surfaces only a genuinely different nonzero PC; and
4. brackets interpreted/JIT calls into static code with `g_call_unit_depth`.

Generated game/BIOS code is unchanged.

## Validation

```bash
cmake -S runtime -B runtime/build -DBUILD_TESTING=ON
cmake --build runtime/build --target \
  call_return_boundary_test starvation_release_policy_test
ctest --test-dir runtime/build --output-on-failure \
  -R 'call_return_boundary|starvation_release_policy'
```

For a live game check, run the diagnostic export to the rumble prompt, select
`On`, and verify that the next screen advances instead of leaving the overlay at
`0x801D2CB0..0x801D2CC0`.
