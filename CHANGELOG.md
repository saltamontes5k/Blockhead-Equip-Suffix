# Changelog

## [0.5d] - 2026-06-16

### Fixed
- `[EquipInvisible]` head/hair slots no longer show missing-mesh errors.
  Hidden slots reference `Data\Meshes\_invisible.nif` — a valid zero-geometry
  NIF that the engine loads successfully and renders nothing.  You must
  provide `_invisible.nif` yourself (place any empty NIF at that path).

## [0.5c] - 2026-06-16

### Changed
- `[EquipInvisible]` now supports slot-specific visibility:
  `formID=Weapon,Shield` hides everything except Weapon and Shield.
  Plain `formID` (no `=`) still hides all slots (backwards compatible).

## [0.5b] - 2026-06-16

### Fixed
- Race group suffix no longer applies non-existent NIF paths (was calling
  `ApplyTESModel` unconditionally regardless of `FileExists` result, causing
  invisible equipment slots)

### Added
- `[EquipInvisible]` INI section — NPC formIDs listed here get all equipment
  slots hidden (renders nothing for each slot).  Uses the same mechanism that
  was causing invisible slots, but intentional.

## [0.5a] - 2026-06-16

### Changed
- Hard dependency on Blockhead — plugin requires Blockhead to load
- All override and non-override paths now pass through Blockhead's handler
- BodyPart=-1 resolved to correct part before logging (same as Blockhead)

### Removed
- Standalone engine original call (`kEngineOriginal`) — Blockhead handles everything
- Standalone fallback code in chain paths
- Unused `TESModel_Ctor` and `kTESModel_Ctor` constants

## [r4] - 2026-06-16

### Added
- Diagnostic logging to `BlockheadEquipSuffix.log` with indented per-override detail
- `[Settings]` INI section with `Logging=1/0` toggle (default: ON)
- Log entries for: NPC formID, source path, constructed override path, file existence, race null warnings, chain/fallback

### Fixed
- File existence check now uses Oblivion's internal FileFinder (same as Blockhead) instead of `GetFileAttributesA`, enabling BSA + loose file lookups
- Race name resolution uses `GetFullName()` (same as Blockhead) instead of `GetEditorID()` for correct handling of mod-added races
- Race group suffix now includes underscore (`_0` instead of `0`)
- Override cascade: NPC suffix → race group suffix → chain to Blockhead

### Changed
- Version bumped to 4
- Build paths updated for sibling xOBSE-master directory layout

## [1.0.0] - 2026-06-15

### Added
- Initial release
- Suffix-based equipment model overrides alongside original NIF files
- Race group system via `[EquipRaceGroups]` INI section
- NPC alias system via `[EquipNPCGroups]` INI section
- Automatic underscore prepending for NPC alias filenames
- Comma-separated multi-formID support for NPC aliases
- Hook chains to Blockhead's handler when no suffix override found
- Added PDB support
