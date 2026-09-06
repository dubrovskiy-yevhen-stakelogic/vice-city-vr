# Vice City VR patch-only source kit

This repository carries only the project-authored delta needed to reproduce the
PC build. It does not redistribute the complete reVC source tree.

Source-kit version: `0.5.5-alpha-pc`.

The fixed upstream baseline is:

- repository: `https://github.com/mrxenginner/reVC.git`
- commit: `026cd10f3fdbd92c089830e5067c4457c53c1b51`

The kit contains:

- `patches/`: text-only modifications to files that exist in the fixed baseline;
- `overlay/`: new project-authored source files;
- `librw/`: the audited source-only renderer fork copied to `vendor/librw`;
- `tools/source-kit/`: audit, assembly, and build-only scripts;
- `patch-manifest.json`: exact paths and SHA-256 values for every patch,
  preimage, result, overlay file, and librw file.

No game files, compiled binaries, signing material, proprietary SDKs, or media
assets belong in this repository.

## Assemble a source tree

Prepare an exact clean checkout first:

```powershell
git clone https://github.com/mrxenginner/reVC.git D:\source\revc-base
git -C D:\source\revc-base checkout --detach 026cd10f3fdbd92c089830e5067c4457c53c1b51
git -C D:\source\revc-base status --porcelain --untracked-files=all
```

The status command must print nothing. Then choose an output path that does not
exist:

```powershell
.\ASSEMBLE_SOURCE.bat -Revc D:\source\revc-base -Out D:\work\vice-city-vr-source
```

Assembly audits the kit, verifies the exact upstream commit and clean state,
exports the exact Git tree without workstation line-ending conversion, validates
the baseline's exact gitlink set, verifies every manifest hash, checks and
applies each patch, then copies the overlay and librw source. The input checkout
is never modified. Existing output paths are refused.

Run the publication audit independently with:

```powershell
.\AUDIT_SOURCE_KIT.bat
```

See [BUILDING.md](BUILDING.md) for compilation and [RELEASING.md](RELEASING.md)
for the release process.

For player-side optional neural rendering, see
[DLSS 5 setup](docs/DLSS5_SETUP.md). NVIDIA runtime and model files, including
any locally modified files used for hardware experiments, are not included.

## Manifest contract

`patch-manifest.json` uses schema version 1:

- `base`: exact repository, branch, commit, and Git tree IDs;
- `patches[]`: path relative to `patches/`, file SHA-256, and `targets[]`;
- every target: upstream-relative path, `preimageSha256`, and
  `postimageSha256`;
- `overlay[]`: output-relative path and SHA-256 relative to `overlay/`;
- `librw[]`: renderer-relative path and SHA-256 relative to `librw/`.

All manifest paths use forward slashes. Additions belong in the overlay, not in
the patch. Empty manifest arrays or hashes that do not match the release files
are rejected by the audit and assembler.
