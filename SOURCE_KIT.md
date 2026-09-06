# Source-kit boundary

This repository is intentionally not a buildable reVC checkout by itself. It
contains only the Vice City VR delta and the permitted renderer source:

- `patches/` modifies files already present in the pinned reVC baseline;
- `overlay/` contains files absent from that baseline;
- `librw/` is the complete source-only MIT renderer snapshot;
- `tools/source-kit/` validates, assembles, and builds in a new directory.

`patch-manifest.json` binds the kit to one upstream commit and tree and records
the SHA-256 of every patch, preimage, postimage, overlay file, and librw file.
`SOURCE_MANIFEST.sha256` covers the distributable kit itself.

The assembled checkout and build output are local products. They may contain
the upstream reVC tree and compiled binaries and therefore must never be copied
back into this repository or published as the source kit.

The pack-specific Modern model downloader is excluded because the external
packs do not yet have a documented redistribution grant. Runtime support for a
locally prepared model set remains present. Generic source utilities are kept
under their separate notice and operate only on files supplied by the user.

For local testing, `tools/modelsets/install-local-profiles.ps1` can prepare two
small, non-destructive IMG overlays from user-supplied folders. Pass
`-OptimizedVegetation` to replace the MODERN vegetation category and
`-XboxVehicles` to add the third vehicle choice, CLASSIC / MODERN / XBOX.
The vegetation installer also repairs the known `Palm_Tree_Leaf` DXT3 alpha
mips in the copied profile so distant crowns do not disappear; it leaves the
user's downloaded source pack untouched. Neither third-party asset pack is
included in this source kit.

Player installation and game launch are deliberately outside the source-kit
scripts. Compilation success is not runtime acceptance.
