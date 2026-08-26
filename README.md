# BlockheadEquipSuffix

A state-driven suffix override plugin for *The Elder Scrolls IV: Oblivion*.

A native OBSE plugin (no scripts) that rewrites the model/texture paths of
**equipment, weapons, and the actor's own body/head assets** by appending a
suffix to the original filename. The suffix is chosen by a declarative rule
engine — faction, class, race, inventory, location, weather, quest stage,
quest variable, or game setting — so you can give different NPCs different
gear and bodies purely by dropping files next to the originals.

Built on **Blockhead** (ShadeMe). Requires only **Blockhead**.

## Features

- **Suffix override engine** — one `[EquipRules]` rule set drives every asset
  type: armor/clothing/weapon meshes, body meshes/textures, head
  meshes/textures, and body EGT.
- **Native clauses** — no OBSEKeywords, no scripts: `fac:`, `race:`, `class:`,
  `npc:`, `inv:`, `ws:`, `cell:`, `qst:`, `qvar:`, `gst:`, `wtr:`.
- **Body/head asset overrides** — per-race and per-NPC override folders, the
  same way Blockhead's body/head overrides work, but driven by the rule
  engine (any race with a `_{suffix}_` file is matched automatically).
- **Opt-in head override** — per-NPC facegen head mesh/texture overrides
  (`[HeadOverride]`).
- **Legacy groups** — `[EquipRaceGroups]`, `[EquipNPCGroups]`,
  `[EquipFactionGroups]`, `[EquipClassGroups]`, `[EquipInvisible]` as in the
  original BES.
- Loads **after** Blockhead so the hook chain is always set up correctly.

## Requirements

1. [Blockhead](https://github.com/ShadeMe/Blockhead)
2. xOBSE (OBSE 0021+)
3. Oblivion 1.2.416

Optional but recommended: [BlockheadEngineFixes](https://github.com/gnarlyman)
(or the equivalent facegen-fix companion plugin) to stabilise the engine's
facegen flood bug before enabling `[HeadOverride]`.

## Installation

1. Install Blockhead.
2. Copy `BlockheadEquipSuffix.dll` and `BlockheadEquipSuffix.ini` to
   `Data\OBSE\Plugins\`.
3. Optionally copy `_invisible.nif` to `Data\Meshes\` (used by
   `[EquipInvisible]`).

## How it works

A rule in `[EquipRules]` maps a **suffix** to one or more clauses. When an
NPC's equipment or body/head asset is being applied, the plugin evaluates the
rules top-to-bottom and the **first rule whose clauses all pass** selects the
suffix. The suffix is appended to the asset filename before its extension:

```
Data\Meshes\Armor\Chainmail\M\Greaves.NIF     -> Greaves_blade.NIF
Data\Textures\Characters\Imperial\HeadHuman.dds -> HeadHuman_blade.dds
```

Whatever file actually exists is used; missing suffixed files fall through to
the vanilla asset. This means a single rule can drive gear *and* body/head
changes at once — the file's location decides what it overrides.

## The rule engine — `[EquipRules]`

```
[EquipRules]
<suffix>=<clause>, <clause>, ...       ; all clauses ANDed
```

Clauses are evaluated against the NPC base form and/or the ref. First rule in
the file that matches wins.

### Clause reference

| Clause | Meaning |
|---|---|
| `npc:<hexformid>` | NPC base form ID matches |
| `race:<name\|hexfid>` | race display name (case-insensitive) or form ID |
| `fac:<hexformid>` | NPC is in the faction (form ID) |
| `class:<hexformid>` | NPC's class matches (form ID) |
| `inv:<hexformid>:<op>:<n>` | inventory count of the item vs `n` (gold = `Gold001`, `0000000F`) |
| `ws:<hexformid>` | actor's current worldspace matches |
| `cell:<hexformid>` | actor's current cell matches |
| `qst:<hexformid>:<op>:<n>` | quest's current stage vs `n` |
| `qvar:<hexformid>:<var\|idx>:<op>:<n>` | quest script **variable** runtime value |
| `gst:<editorID>:<op>:<n>` | numeric **Game Setting** value |
| `wtr:<hexformid\|name>` | current weather form ID or precipitation (`none/clear/cloudy/rain/snow`) |

Operators: `== != < <= > >=`.

`qvar:` and `gst:` compare as integers. Quest-variable names are
case-insensitive.

### Equipment / weapon examples

```
; Rich player -> Cuirass_rich.nif / WaalxSword_rich.nif
rich=inv:0000000F:>:10000

; In the Oblivion planes
plane=ws:0001A694

; Rainy weather
rain=wtr:rain

; Quest branch (Glarthir / Paranoia, stage 30 = help, 40 = told the guards)
glar_help=npc:0001C1B2,qst:00023D80:>=:30,qst:00023D80:<:40
glar_guard=npc:0001C1B2,qst:00023D80:>=:40

; GMST-gated gear
slowmo=gst:fGlobalTimeMultiplier:==:0.5

; Quest-variable-gated gear
paranoia=qvar:00023D80:prisonerCounter:>=:2

; Race-variant weapon (High Elf holds WaalxSword -> WaalxSword_elf.nif)
elf=race:High Elf

; NPC-variant weapon (the player -> WaalxSword_pconly.nif)
wpn_pconly=npc:00000007
```

### Body / head asset examples

```
; BLADES demo: every Blades member gets a scarred head texture.
; Faction FormID 000274AB (verified). Author:
;   Textures\Characters\Imperial\HeadHuman_blade.dds
;   Textures\Characters\DarkElf\HeadDarkElf_blade.dds
;   ... (per race) ...
blade=fac:000274AB

; Race-variant body texture (Orcs)
orcbody=race:Orc
;   -> Textures\Characters\Orc\Male\UpperBodyMale_orcbody.dds

; Body mesh via the per-race override folder (one rule, any race)
brute=fac:0002347A
;   -> Meshes\Characters\BodyAssetOverrides\PerRace\M\Orc_brute_UpperBody.NIF
;   -> Meshes\Characters\BodyAssetOverrides\PerRace\M\Imperial_brute_UpperBody.NIF
```

## Body / head asset overrides

Body and head assets are resolved at Blockhead's override points (body
texture, body model, body EGT, and the facegen head path). For a matched
suffix, the plugin tries, in order:

1. **Direct suffix** — `{base}_{suffix}.{ext}` next to the original.
2. **Per-race override folder** —
   `Data\Meshes\Characters\{Body|Head}AssetOverrides\PerRace\{M|F}\{Race}_{suffix}_{Component}.{ext}`
   (also under `Data\Textures\...` for textures). Any race with a
   `_{suffix}_` file is matched automatically — no per-race rules needed.
3. **Per-NPC override folder** —
   `...\PerNPC\{Plugin}\{FormID:08X}_{suffix}_{Component}.{ext}`.
4. Otherwise the chain passes through to Blockhead's own per-race/NPC/gender
   override system.

Body components: `UpperBody`, `LowerBody`, `Hand`, `Foot`, `Tail`.
Head components: `Head`, `EarsMale`, `EarsFemale`, `Mouth`, `TeethLower`,
`TeethUpper`, `Tongue`, `EyesLeft`, `EyesRight`.

### `[HeadOverride]`

The facegen head override runs inside the live facegen pipeline and is opt-in:

```
[HeadOverride]
Enabled=1
```

While OFF the facegen detour is a pure passthrough. Enable it only with real
`_blade`-style head assets, and pair it with an engine facegen fix (see
Requirements) for stability.

## Legacy groups

```
[EquipRaceGroups]     0=Orc,Nord,Dremora        ; -> Greaves_0.nif
[EquipNPCGroups]      pconly=00000007           ; -> Greaves_pconly.nif
[EquipFactionGroups]  <suffix>=<hexformid>,...
[EquipClassGroups]    <suffix>=<hexformid>,...
[EquipInvisible]      <formid>[=<slots to keep>]
```

`[EquipInvisible]` hides equipment slots on specific NPCs. A plain formID
hides everything; `formid=Weapon` keeps only the weapon visible. Slot names:
`Head Hair UpperBody LowerBody Hand Foot RightRing LeftRing Amulet Weapon
BackWeapon SideWeapon Quiver Shield Torch Tail`.

## Compatibility

- **Blockhead** — required; the plugin chains through Blockhead's equipment
  and body/head override systems. When both the plugin and Blockhead have an
  override for the same body/head asset, Blockhead's per-race/NPC/gender
  folders win (the plugin's suffixed path is preserved when Blockhead has no
  override).
- **BES / BES-Keywords** — do not install alongside (same hooks).
- **BlockheadEngineFixes** — compatible; recommended for facegen stability.

## Build

Requires Visual Studio 2022 with the xOBSE SDK (as a sibling of this repo's
parent). Open `src\BlockheadEquipSuffix.sln` and build Release/Win32.

## License

MIT — see LICENSE
