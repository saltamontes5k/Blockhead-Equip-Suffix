# Changelog

## [0.5a] - 2026-06-16

### Changed
- Hard dependency on Blockhead — plugin requires Blockhead to load, no reinventing the wheel
- All override and non-override paths now pass through Blockhead's handler, see above
- BodyPart=-1 resolved to correct part before logging (same as Blockhead)
- Realism with version number

### Removed
- Standalone engine original call (`kEngineOriginal`) — Blockhead handles everything
- Standalone fallback code in chain paths
- Unused `TESModel_Ctor` and `kTESModel_Ctor` constants

## [R4] - 2026-06-16

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
- Version bumped to Alpha4
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
