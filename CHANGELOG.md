# Changelog

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
