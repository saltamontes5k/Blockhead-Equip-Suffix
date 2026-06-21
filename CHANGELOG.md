# Changelog

## [0.5dd] - 2026-06-21

### Added
- Planned: native BreakArmor replacement (`_ba{N}` suffix for health-based deterioration)
- `FlushInstructionCache` in all WriteJump calls for reliable code patching
- `IsAddressInModule` checks to verify Blockhead hooks before chaining
- `__try/__except` guard in `ReadJumpTarget` for crash-safe hook reading
- `_BitScanForward` for efficient partMask resolution (replaces 16-iteration loop)
- Precomputed indent buffer for faster logging

### Changed
- Heavy refactor into single-file architecture (`Main.cpp`)
- `atoi` replaced with `strtol` (proper error detection) for INI parsing
- HandleBaseBody removed — base body invisibility dropped for stability

### Removed
- All face hook implementations (crashed with other plugins)
- All faction system code (scope reduction)
- `CoverInvisibleBody` setting and implementation
- Body model trampoline hook
- Vanity system (dropped from plans)

### Fixed
- Stack corruption in function hook stub (double cleanup of __stdcall params)
- Instruction boundary alignment in trampoline (instruction-length decoder)

## [0.5d] - 2026-06-16

### Fixed
- `[EquipInvisible]` head/hair slots no longer show missing-mesh errors.
  Hidden slots reference `Data\Meshes\_invisible.nif` — a valid zero-geometry
  NIF that the engine loads successfully and renders nothing.

## [r4] - 2026-06-16

### Added
- Diagnostic logging to `BlockheadEquipSuffix.log` with indented per-override detail
- `[Settings]` INI section with `Logging=1/0` toggle (default: ON)
- File existence check uses Oblivion's internal FileFinder (BSA + loose file lookups)
- Race name resolution uses `GetFullName()` for mod-added race support

## [1.0.0] - 2026-06-15

### Added
- Initial release: equipment model suffix overrides alongside original NIFs
- Race group system (`[EquipRaceGroups]`)
- NPC alias system (`[EquipNPCGroups]`)
- Hook chains to Blockhead when no suffix override found
