# Blockhead EquipSuffix  0.5a

A lightweight companion plugin for **Blockhead** that provides suffix-based equipment model overrides alongside the original NIF files.

## How it works

Instead of a separate override directory tree, override files live alongside the originals.
A suffix (race group number or NPC alias) is appended to the filename before the extension.

```
Meshes\Armor\Iron\M\Greaves.nif        ← original
Meshes\Armor\Iron\M\Greaves_0.nif      ← race group 0 override
Meshes\Armor\Iron\M\Greaves_pconly.nif  ← NPC alias "_pconly" override
```

The plugin hooks `TESBipedModelForm::GetBodyPartModel`, computes the suffix path, and
passes it to **Blockhead's handler** — Blockhead calls the engine and handles all
downstream features (scripted overrides, per-NPC overrides, etc.).

## 0.5a — now actually depends on Blockhead

All override paths go through Blockhead's `SwapEquipmentModelData` handler.
This plugin only computes WHICH suffix file to use; Blockhead does the rest.

## 0.5a Changes

- **Diagnostic logging**: Writes to `Data\OBSE\Plugins\BlockheadEquipSuffix.log`.
  Every override attempt logs NPC formID, original model path, constructed suffix
  path, file existence check result, and chain/fallback decisions.
- **`[Settings]` Logging toggle**: Set `Logging=0` in the INI to disable logging.
  Defaults to ON.
- **FileFinder fix**: Uses Oblivion's internal FileFinder (same as Blockhead) instead
  of `GetFileAttributesA`, ensuring correct lookup across BSAs and loose files.
- **Race name resolution**: Uses `GetFullName()` (same as Blockhead) instead of
  `GetEditorID()` for correct handling of mod-added races.
- **TESModel construction**: Properly calls the TESModel constructor to set up the
  vftable, avoiding undefined behavior in the model cache.

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
```

Race groups match against the race's display name (e.g. "Nord", "Orc").
NPC aliases are plain names — an underscore is prepended automatically in the
filename (e.g. alias `pconly` → `_pconly`).

## Debugging

When `Logging=1` (default), every suffix lookup is written to
`Data\OBSE\Plugins\BlockheadEquipSuffix.log`.  Check this log if an override
isn't being picked up — it shows exactly which path was constructed and whether
the file was found.

## Building

Requires:
- Visual Studio 2022 with C++ desktop workload
- xOBSE SDK (clone as `xOBSE-master` sibling to `blockheadsuffix-v05`)
- Common library (included with xOBSE)

```
msbuild BlockheadEquipSuffix.sln /p:Configuration=Release /p:Platform=Win32
```

## Dependencies

- Blockhead (OBSE plugin) — must be loaded
- Latest xOBSE

## Thanks
ShadeMe for Blockhead, xObse and OBSE team for Xobse, Bethsoft for Oblivion, GBR for LMP's sizeable cloth and iamnoone's Mesh Extended Swap System as direct inspiration for this mod.
