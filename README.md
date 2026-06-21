# Blockhead EquipSuffix

A suffix-based equipment model override plugin for *The Elder Scrolls IV: Oblivion*.
Extends **Blockhead** (ShadeMe) — requires Blockhead to be installed.

## How it works

Instead of replacing or repathing NIF files, EquipSuffix appends a suffix to the
original equipment model filename before `.nif`. Files sit alongside the originals
in the same directory.

```
Data\Meshes\Armor\Iron\Greaves.nif         (original)
Data\Meshes\Armor\Iron\Greaves_0.nif       (race group suffix)
Data\Meshes\Armor\Iron\Greaves_pconly.nif  (NPC alias suffix)
Data\Meshes\Armor\Iron\Greaves_0_ba2.nif   (race group + break armor) [planned]
```

The override cascade is:

1. **EquipInvisible** → `_invisible.nif` (slot hidden, equipment only)
2. **EquipNPCGroups** → `_{alias}.nif` (matched by NPC formID)
3. **EquipRaceGroups** → `_{group}.nif` (matched by race name)
4. **EquipBreakArmor** → `_ba{N}.nif` (planned — health-based deterioration)
5. **Chain to Blockhead** → Blockhead's own override system takes over

## Planned: BreakArmor Replacement

The OBSE-script-based BreakArmor mod will be replaced by a native C++ implementation
within this plugin. Instead of per-frame polling, dummy refs, and action queues, break
stages will be evaluated at equip-time via the existing suffix system.

- `_ba1`, `_ba2`, `_ba3` suffixes appended after grouping suffixes
- Item health read from `ExtraDataList::Health` — no polling needed
- Stacks with race/NPC group suffixes: `Cuirass_0_ba2.nif`
- No OBSE scripts, no dummy forms, no MESS protocol dependencies

## Installation

1. Install [Blockhead](https://github.com/ShadeMe/Blockhead)
2. Copy `BlockheadEquipSuffix.dll` to `Data\OBSE\Plugins\`
3. Copy `BlockheadEquipSuffix.ini` to `Data\OBSE\Plugins\`
4. Optionally copy `_invisible.nif` to `Data\Meshes\` (used by EquipInvisible)

## INI Sections

### `[EquipRaceGroups]`

```
0=Orc,Nord,Dremora
```

Equipment on Orcs, Nords, and Dremora will try `{name}_0.nif`.

### `[EquipNPCGroups]`

```
pconly=00000007
```

Equipment on formID `00000007` (the player) will try `{name}_pconly.nif`.

### `[EquipInvisible]`

Plain formID hides all equipment slots. FormID with slot list hides all EXCEPT
those listed:

```
002353=Weapon
```

Only the weapon renders on NPC 002353. All other slots use `_invisible.nif`.

Slot names: Head, Hair, UpperBody, LowerBody, Hand, Foot, RightRing, LeftRing,
Amulet, Weapon, BackWeapon, SideWeapon, Quiver, Shield, Torch, Tail

## Compatibility

- **Blockhead** — required (equipment hook chaining)
- **EngineBugFixes** — compatible (no address overlap)
- **Oblivion Reloaded** — compatible (no address overlap)
- **BreakArmor (script)** — will be obsoleted by planned native replacement
- **SetBody / SetBodyMess** — fully compatible (operates inside Blockhead, different layer)

## Build

Requires Visual Studio 2022 with xOBSE SDK. Open `src\BlockheadEquipSuffix.sln`
and build Release/Win32.

## License

MIT — see LICENSE
