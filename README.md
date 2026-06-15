# Blockhead EquipSuffix

A companion OBSE plugin for Blockhead that provides suffix-based equipment model overrides.

## How it works

Instead of a separate override directory tree, override files live alongside the originals.
A suffix (race group number or NPC alias) is appended to the filename before the extension.

```
Meshes\Armor\Iron\M\Greaves.nif        ← original
Meshes\Armor\Iron\M\Greaves0.nif       ← race group 0 override
Meshes\Armor\Iron\M\Greaves_pconly.nif  ← NPC alias "_pconly" override
```

The plugin hooks `TESBipedModelForm::GetBodyPartModel` and chains to Blockhead's
existing handlers when no suffix override is found.

## Configuration

Place `BlockheadEquipSuffix.ini` in `Data\OBSE\Plugins\`:

```ini
[EquipRaceGroups]
0=Orc,Nord,Dremora
1=Imperial,Breton,Redguard

[EquipNPCGroups]
;pconly=00000007
;bigguys=00037FF8,000222B6
;mages=00034E16,00016487

```

Race groups use the race's EditorID (the name shown in the Construction Set).
NPC aliases are plain names — an underscore is prepended automatically.

## Building

Requires:
- Visual Studio 2022 with C++ desktop workload
- xOBSE SDK (clone to `..\xOBSE-master\` relative to solution)
- Common library (included with xOBSE)

```
msbuild BlockheadEquipSuffix.sln /p:Configuration=Release /p:Platform=Win32
```

## Dependencies

- Blockhead (OBSE plugin) — must be loaded first
- Latest xOBSE

## Thanks
ShadeMe for Blockhead, xObse and OBSE team for Xobse, Bethsoft for Oblivion, GBR for LMP's sizeable cloth and iamnoone's Mesh Extended Swap System as direct inspiration for this mod.