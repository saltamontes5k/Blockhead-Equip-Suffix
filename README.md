# Blockhead EquipSuffix  v0.5d

A lightweight companion plugin for **Blockhead** that provides suffix-based equipment model overrides alongside the original NIF files.

## How it works

Instead of a separate override directory tree, override files live alongside the originals.
A suffix (race group number or NPC alias) is appended to the filename before the extension.

```
Meshes\Armor\Iron\M\Greaves.nif        ← original
Meshes\Armor\Iron\M\Greaves_0.nif      ← race group 0 override
Meshes\Armor\Iron\M\Greaves_pconly.nif ← NPC alias "_pconly" override
```

The plugin hooks `TESBipedModelForm::GetBodyPartModel`, computes the suffix path, and
passes it to **Blockhead's handler** — Blockhead calls the engine and handles all
downstream features (scripted overrides, per-NPC overrides, etc.).

## Depends on Blockhead

All override paths go through Blockhead's `SwapEquipmentModelData` handler.
This plugin only computes WHICH suffix file to use; Blockhead does the rest.

## Features

- **Script priority**: Blockhead's `RegisterEquipmentOverrideHandler` system fires
  inside every suffix override call and can override suffix results — scripts always win
- **Suffix cascade**: NPC alias → race group → fallthrough to Blockhead
- **Diagnostic logging**: Per-override log to `BlockheadEquipSuffix.log` with INI toggle
- **Race groups**: Numbered suffixes per race display name
- **NPC aliases**: Named suffixes per NPC formID
- **Slot-specific invisibility**: `[EquipInvisible]` section with `formID=Slot1,Slot2` syntax
- **All vanilla checks**: Uses Oblivion's FileFinder, `GetFullName()` for races, proper TESModel construction

## Configuration

Place `BlockheadEquipSuffix.ini` in `Data\OBSE\Plugins\`:

```ini
[Settings]
Logging=1

[EquipRaceGroups]
0=Orc,Nord,Dremora
1=Imperial,Breton,Redguard

[EquipNPCGroups]
;pconly=00000007
;bigguys=00037FF8,000222B6
;mages=00034E16,00016487

[EquipInvisible]
; <NPCFormID> or <NPCFormID>=Slot1,Slot2,...
;00000007=Weapon,Shield
```

## Priority order (per-equipment-slot)

1. **Invisible check** — `[EquipInvisible]` → slot hidden
2. **Blockhead scripts** — registered `EquipmentOverride` handlers fire via Blockhead
3. **NPC suffix** — `[EquipNPCGroups]` → file found? → apply
4. **Race suffix** — `[EquipRaceGroups]` → file found? → apply
5. **Chain to Blockhead** — normal engine behaviour (per-NPC overrides, scripted fallback)

Steps 2–4 all pass through Blockhead's `SwapEquipmentModelData`, so Blockhead's
scripted handlers fire for every suffix override and can override the result.

Blockhead's script system runs inside every suffix override step (2–3) via
`CallBlockhead`.  If a registered script replaces the body part model, it
overrides the suffix result — scripts have the highest effective priority.

## `_invisible.nif`

For `[EquipInvisible]`, provide a valid zero-geometry NIF at
`Data\Meshes\_invisible.nif`.  The engine loads it successfully and renders
nothing — no missing-mesh error.  Included in `src\Data\meshes\_invisible.nif`.

## Plans

- **Faction suffix system** — rank-aware suffixes per faction formID
  (`[EquipFactionGroups]`)
- **Vanity bypass** — per-NPC and per-race slot hiding that shows the
  race's default body underneath (`[EquipVanityBypass]`,
  `[EquipRacialVanityBypass]`)

## Building

Requires:
- Visual Studio 2022 with C++ desktop workload
- xOBSE SDK (clone as `xOBSE-master` sibling to this folder)
- Common library (included with xOBSE)

```
msbuild BlockheadEquipSuffix.sln /p:Configuration=Release /p:Platform=Win32
```

## Dependencies

- Blockhead (OBSE plugin) — must be loaded
- Latest xOBSE

## Thanks
ShadeMe for Blockhead, xObse and OBSE team for Xobse, Bethsoft for Oblivion, GBR for LMP's sizeable cloth and iamnoone's Mesh Extended Swap System as direct inspiration for this mod.