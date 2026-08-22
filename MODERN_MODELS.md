# Optional Modern models

Vice City VR v0.5.2 can use an isolated `modelsets\modern` overlay. No GTA or
third-party assets are stored in this repository: the tool builds a personal
copy from your legal 2003 PC installation and the external packs on your own
computer.

## One-button setup

1. Keep at least 24 GB free on the drive used for the temporary workspace.
2. Double-click **PREPARE_MODERN_MODELS.bat**.
3. Select the folder containing your legal Vice City installation when the
   folder picker opens.
4. Confirm the approximately 4 GB download and wait for **COMPLETE**.
5. Fully restart Vice City VR and open **VR Menu > Model Assets**.

The wizard downloads and verifies the two tested source archives, extracts
them, builds a staged overlay, verifies the result, and only then replaces an
older `modelsets\modern` folder. It never overwrites the original `models` or
`txd` directories.

The download and extraction cache is kept in:

```text
C:\VCVRBuild\modern-assets
```

Rerunning the button reuses verified downloads, verified extractions, and an
already complete current overlay. Interrupted downloads resume automatically.
An old or incomplete overlay is not removed until its replacement has passed
the final hash and structure checks.

## What the wizard downloads

- [GTA VC HD + Weapons](https://drive.google.com/file/d/1Swe1dVWDnKz8ad51y8L0ihPWVCxmFRYj/view)
- [Mods / Atmosphere](https://drive.google.com/file/d/1y9KpKjLSna76bjz1Lf2DzP0G4AnkN_2d/view)

HD Vegetation is deliberately not downloaded or installed. Its palm models
are unusually expensive in VR, so Vegetation / Palms remains Classic.

Pinned archives used by the one-button wizard:

| Archive | Size | SHA-256 |
| --- | ---: | --- |
| GTA VC HD + Weapons.zip | 1,878,280,127 | `81A7962479752F3A07004A2E12964815435CAF4B0A0F637EE982460171A5D94C` |
| Mods.zip | 2,377,186,981 | `F33031091DBE50A3BEDCEEFAF3FDE6F6DDD96F9841AABE1ECD3713818DED9725` |

## Command-line use

The same wizard can be run from PowerShell:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ".\tools\prepare-modern-models.ps1" -GameDir "C:\Games\Grand Theft Auto Vice City"
```

Read-only preflight, with no download, extraction, build, or game-file change:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ".\tools\prepare-modern-models.ps1" -GameDir "C:\Games\Grand Theft Auto Vice City" -DryRun
```

Advanced users may supply already downloaded verified archives with
`-HdArchive` and `-ModsArchive`. The lower-level builder remains in
`tools\modelsets\build-modern-modelset.ps1`.

## Safety and troubleshooting

- Select the folder containing `models\gta3.img`, `models\gta3.dir`, and
  `models\generic.txd`.
- Do not point the tool at `models` itself.
- Do not close the window while a multi-gigabyte archive is being built.
- On failure, read the red error and the diagnostic log path printed at the
  bottom. Verified downloads and extracted inputs stay cached for the retry.
- To remove the optional overlay, first switch every Model Assets category to
  Classic, exit the game, and delete only `modelsets\modern`.

Generated overlays contain data derived from the player's game and external
mods. They are for that player's local personal installation and must not be
uploaded or redistributed.
