# Building the optional Modern asset set

Vice City VR v0.5.0 supports an isolated Modern asset overlay assembled locally
from a player's legal Vice City installation and three separately downloaded
mods. The Vice City VR release does not contain these assets.

The supplied build script creates:

~~~text
<GameDir>\modelsets\modern\
    vegetation_models.txt
    models\gta3.img
    models\gta3.dir
    models\...
    txd\...
~~~

It does not replace the original **models** or **txd** directories. The tested
result is approximately 3.29 GiB, and the build temporarily needs additional
space while rebuilding the IMG archive. Keep at least 12 GB free on the drive
containing the game, in addition to space for the downloaded archives.

## What you need

- A legal installation of the original 2003 PC version of *Grand Theft Auto:
  Vice City*.
- Vice City VR v0.5.0 already extracted into that installation.
- The original **models\gta3.img** and **models\generic.txd** from your legal
  game installation.
- PowerShell 5.1 or newer.
- 7-Zip, WinRAR, or another extractor that supports password-protected ZIP
  archives.

## Download the three source packs

1. [GTA VC HD + Weapons](https://drive.google.com/file/d/1Swe1dVWDnKz8ad51y8L0ihPWVCxmFRYj/view)
2. [Mods](https://drive.google.com/file/d/1y9KpKjLSna76bjz1Lf2DzP0G4AnkN_2d/view)
3. [Vegetation in HD](https://libertycity.net/files/gta-vice-city/126557-vegetation-in-hd.html)

The password for the **Vegetation in HD** archive is:

~~~text
libertycity
~~~

Extract each download into a separate folder. Do not merge the three extracted
folders and do not copy their complete contents into the Vice City directory.
Some archives contain ASI loaders, DLLs, configuration files, or other pieces
intended for the original executable. Vice City VR does not load those
components. The builder selects only the required model, texture, collision,
and wheel assets.

## Verify the tested downloads

These are the exact files used to build and test the v0.5.0 Modern overlay:

| Archive | Size in bytes | SHA-256 |
| --- | ---: | --- |
| GTA VC HD + Weapons.zip | 1,878,280,127 | 81A7962479752F3A07004A2E12964815435CAF4B0A0F637EE982460171A5D94C |
| Mods.zip | 2,377,186,981 | F33031091DBE50A3BEDCEEFAF3FDE6F6DDD96F9841AABE1ECD3713818DED9725 |
| 1557511944_1557494130_hd-vegetation.zip | 21,815,644 | D5F8CC260913046C9C11DE0570FD2BA20C5B97EDED5DA2C43AB834992E356AFB |

To calculate a hash in PowerShell:

~~~powershell
Get-FileHash -Algorithm SHA256 "C:\Downloads\GTA VC HD + Weapons.zip"
Get-FileHash -Algorithm SHA256 "C:\Downloads\Mods.zip"
Get-FileHash -Algorithm SHA256 "C:\Downloads\1557511944_1557494130_hd-vegetation.zip"
~~~

A source owner may update a file behind the same link. A different hash does
not automatically mean the file is malicious, but it does mean that it is not
the exact input tested for v0.5.0. Do not continue unless you trust the source
and understand the difference.

## Run the builder

Open PowerShell in the release's **tools\modelsets** directory. Change all four
example paths to match your computer. First run the read-only preflight:

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ".\build-modern-modelset.ps1" -GameDir "C:\Games\Vice City VR" -HdPack "C:\Downloads\GTA VC HD + Weapons" -AtmospherePack "C:\Downloads\Mods" -HdVegetation "C:\Downloads\1557511944_1557494130_hd-vegetation" -VerifyOnly
~~~

It validates the source folders and archive structure, prints source hashes,
and writes nothing. If verification passes, run the build command:

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ".\build-modern-modelset.ps1" -GameDir "C:\Games\Vice City VR" -HdPack "C:\Downloads\GTA VC HD + Weapons" -AtmospherePack "C:\Downloads\Mods" -HdVegetation "C:\Downloads\1557511944_1557494130_hd-vegetation"
~~~

The arguments mean:

- **GameDir**: the directory containing **reVC.exe**, **gta-vc.exe**, and the
  original game-data folders.
- **HdPack**: the folder into which GTA VC HD + Weapons was extracted.
- **AtmospherePack**: the folder into which Mods was extracted.
- **HdVegetation**: the folder into which Vegetation in HD was extracted.

You may point at the outer extraction folders. The script searches their
normal nested directory layouts.

The build can take a while because it copies and rebuilds a multi-gigabyte IMG
archive, merges the required vegetation textures, creates mip chains, and
compresses compatible heavy textures. Do not close PowerShell until it reports
that the Modern model set is ready.

## Verify the result

Confirm that these files exist:

~~~text
<GameDir>\modelsets\modern\models\gta3.img
<GameDir>\modelsets\modern\models\gta3.dir
<GameDir>\modelsets\modern\vegetation_models.txt
<GameDir>\modelsets\modern\BUILD_INFO.txt
~~~

The IMG and DIR files must exist as a pair. The vegetation manifest is required
to switch vegetation independently from the rest of the world.

If the build fails, do not manually copy partial output into the original
**models** directory. Correct the reported source path or archive problem and
run the builder again.

If a previous locally built `modelsets\modern` directory already exists, the
builder stops instead of overwriting it. Keep that copy, rename/delete it
manually, or rerun the same command with `-Force`. `-Force` still completes and
validates a separate staged build before replacing the previous overlay.

## Select asset categories in VR

Start **reVC.exe** and open:

**VR Menu > Model Assets**

The menu provides a recommended preset and five independent categories:

| Category | What it controls |
| --- | --- |
| World / Buildings | Map geometry and ordinary world objects |
| Vegetation / Palms | Trees, palms, bushes, and plants listed in the generated manifest |
| Vehicles | Vehicle models, wheels, materials, and associated collision data |
| Pedestrians | Pedestrian models |
| Weapons | Weapon models and the matching Modern calibration profile |

Changes are saved immediately but loaded only during the next startup. Fully
exit the game and start **reVC.exe** again after changing any asset category.

## Recommended performance profile

Use Modern vehicles, weapons, pedestrians, and world assets as desired, but
keep:

**Vegetation / Palms: Classic**

Classic vegetation is the fresh v0.5.0 default and the recommended profile. In
the tested pack, several HD palm models contain dramatically more geometry than
the original models and are placed hundreds of times across the city. They can
raise scene GPU time even when looking toward geometry hidden behind walls.

Render Scale, DLSS, DLAA, FSR 2, and VRS primarily affect pixel work. They
cannot remove the additional triangles or material passes submitted by an HD
palm model. Modern vegetation remains available for users who prefer it and
have enough GPU headroom.

Existing **vr_settings.ini** files are not overwritten by an update. Users of
earlier test builds should manually confirm **Vegetation / Palms: Classic**.

## Occlusion culling

Occlusion culling is enabled by default and can substantially reduce the number
of buildings submitted behind nearby walls. It is independent from the
Classic/Modern category choices.

If one eye briefly jumps or flickers, or geometry is incorrectly hidden, open:

**VR Menu > Graphics > Occlusion Culling**

Set it to **Off**. The setting applies immediately and is saved for future
starts.

## Remove the Modern overlay

Switch the recommended preset to Classic, exit the game, and delete only:

~~~text
<GameDir>\modelsets\modern
~~~

Do not delete the game's original **models** or **txd** directories.

## Legal and redistribution notice

All original Vice City data and third-party replacement content remain the
property of their respective owners. The three source packs are downloaded
through the exact external links used for the tested inputs and are not
distributed by Vice City VR. The two Google Drive downloads contain no
verifiable author or licence metadata in the tested folders; the links are
recorded for reproducibility, not as a claim of provenance or redistribution
permission. Vegetation in HD is credited on its download page to **lenol03**.

The generated Modern **gta3.img** contains data derived from the player's
original game installation and third-party mods. It is for the player's local,
personal installation only. Do not upload, share, mirror, sell, or include the
generated **modelsets\modern** directory in another release.

The links are provided for convenience and do not imply endorsement,
affiliation, or permission to redistribute their contents. Follow the terms set
by each source and obtain the game and optional mods legally.
