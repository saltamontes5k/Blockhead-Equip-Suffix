# Changelog

## [Unreleased]

### Fixed
- **Facegen head overrides** — read `FaceGenRenderState::headModels` /
  `headTextures` as `FaceGenPointerArray` (vtable/data/capacity/firstFree)
  instead of treating the vtable as a `TESModel**` array. This was the root
  cause of the corrupt head path.
- **Race tint overwrite** — removed the `eyeLeft`/`eyeRight` writes at
  `+0xB8/+0xBC`; those fields are `raceFaceTextureData` /
  `raceFaceTintData`, not eye models.
- **`gst:` float comparisons** — parse the right-hand side with `strtod()`
  and compare floats with an epsilon; fractional values such as `0.5` now
  work instead of truncating to `0`.
- **Per-NPC override folder** — reject `GetModIndex() == 0xFF` before
  indexing `modsByID[0xFF]` (out of bounds).
- **Head override default** — align code with README/INI: opt-in, default
  `Enabled=0`.
- **Hook installation** — validate discovered stubs/handlers with
  `VirtualQuery`, check `VirtualProtect` failures, and skip the facegen
  detour when head overrides are disabled.
- **Parser** — reject unknown operators instead of silently treating them
  as `==`.

## [1.0.0] - 2026-08-09

First release.

### Added
- **Suffix override engine** (`[EquipRules]`) — declarative, first-match-wins
  rule set applied to equipment, weapons, and body/head assets.
- **Native clauses**: `npc:`, `race:`, `fac:`, `class:`, `inv:`, `ws:`,
  `cell:`, `qst:`, `qvar:`, `gst:`, `wtr:` — no OBSEKeywords, no scripts.
- **Body/head asset overrides** — direct suffix, per-race override folder,
  per-NPC override folder, chaining through Blockhead.
- **`[HeadOverride]`** — opt-in facegen head model/texture overrides.
- **Legacy groups** — `[EquipRaceGroups]`, `[EquipNPCGroups]`,
  `[EquipFactionGroups]`, `[EquipClassGroups]`, `[EquipInvisible]`.
- Loads after Blockhead (alphabetical order) so the hook chain is correct.

### Notes
- Requires only Blockhead. Head overrides should be paired with an engine
  facegen fix (e.g. BlockheadEngineFixes) for stability.
