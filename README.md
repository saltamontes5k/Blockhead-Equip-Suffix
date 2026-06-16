# Blockhead EquipSuffix  v0.5d

A lightweight companion plugin for **Blockhead** that provides suffix-based equipment model overrides alongside the original NIF files.

## How it works

Instead of a separate override directory tree, override files live alongside the originals.
A suffix (race group number, NPC alias, or faction alias) is appended to the filename before the extension.

```
Meshes\Armor\Iron\M\Greaves.nif              ← original
Meshes\Armor\Iron\M\Greaves_0.nif            ← race group 0
Meshes\Armor\Iron\M\Greaves_pconly.nif       ← NPC alias "pconly"
Meshes\Armor\Iron\M\Greaves_FightersGuild.nif ← faction suffix
```

The plugin hooks `TESBipedModelForm::GetBodyPartModel`, computes the suffix path, and
passes it to **Blockhead's handler** — Blockhead calls the engine and handles all
downstream features (scripted overrides, per-NPC overrides, etc.).

## Depends on Blockhead

All override paths go through Blockhead's `SwapEquipmentModelData` handler.
This plugin only computes WHICH suffix file to use; Blockhead does the rest.

## Features

- **Suffix cascade**: NPC alias → faction rank → race group → fallthrough to Blockhead
- **Diagnostic logging**: Per-override log to `BlockheadEquipSuffix.log` with INI toggle
- **Race groups**: Numbered suffixes per race display name
- **NPC aliases**: Named suffixes per NPC formID
- **Faction groups**: Rank-aware suffixes per faction formID (highest rank wins, ties broken by INI order)
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

[EquipFactionGroups]
; <FactionFormID>=<minRank>,<Suffix>
;0001E66C=0,FightersGuild
;000223C8=1,MagesGuild

[EquipInvisible]
; <NPCFormID> or <NPCFormID>=Slot1,Slot2,...
;00000007=Weapon,Shield
```

## Priority order (per-equipment-slot)

1. **Invisible check** — if NPC in `[EquipInvisible]` and slot not visible → hide
2. **NPC suffix** — `[EquipNPCGroups]` → file found? → apply
3. **Faction suffix** — `[EquipFactionGroups]` → highest rank match → file found? → apply
4. **Race suffix** — `[EquipRaceGroups]` → file found? → apply
5. **Chain to Blockhead** — normal engine behaviour

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