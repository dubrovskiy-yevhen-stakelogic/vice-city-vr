# Patch-only release procedure

Releases are produced from a fixed upstream commit and a positive file
allowlist. Never copy a complete development checkout into this repository.

## 1. Prepare the delta

Use a disposable worktree at
`026cd10f3fdbd92c089830e5067c4457c53c1b51`.

- Put text modifications to existing upstream files in one cumulative patch.
- Put new project-authored source files under `overlay/` using their final
  upstream-relative paths.
- Put only audited librw source, build metadata, and license files under
  `librw/`.
- Exclude generated hashes, generated projects, build output, caches, media,
  game data, executables, libraries, SDK payloads, and local configuration.
- Do not patch localization data or the EAX source subtree.

Before accepting a patch, run `git apply --check` against a fresh exact
upstream checkout and inspect every `diff --git` target.

For an existing kit, the manifest-driven helper creates a separate candidate:

```powershell
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File `
  .\tools\source-kit\regenerate-patch-kit.ps1 `
  -Source D:\work\reviewed-pc-source -Revc D:\source\revc-base `
  -Out D:\work\vice-city-vr-kit-candidate
```

It reads only the existing patch/overlay/librw allowlists from the development
tree. New files require explicit `-NewPatchPaths`, `-NewOverlayPaths`, or
`-NewLibrwPaths` arrays of forward-slash relative paths. Missing allowlisted
files fail instead of silently deleting release content. Overlay additions must
be absent from the pinned upstream. Existing patch preimages remain bound to
that exact upstream, and all patches are regenerated as one cumulative text
delta. Development text in patch targets is normalized to LF to match the
deterministic upstream export; overlay/librw bytes are preserved.

The helper never changes either input and refuses an existing output path. It
updates both manifests and audits the candidate. A failed candidate is retained
for inspection, not accepted as a release. Review its diff before replacement;
the old full-tree synchronization scripts are not valid for this kit.

## 2. Regenerate the manifest

Fill `patch-manifest.json` from the final release files:

- hash each file in `patches/`, `overlay/`, and `librw/` with SHA-256;
- list every patched path exactly once;
- record each patched file's SHA-256 in the untouched upstream checkout as
  `preimageSha256`;
- apply the complete patch series and record the final SHA-256 as
  `postimageSha256`;
- sort all arrays by path to keep reviews deterministic.

No wildcard, optional, or machine-specific manifest entries are allowed.

After every release-file change, regenerate the kit-wide manifest:

```powershell
.\tools\source-kit\update-source-manifest.ps1
```

## 3. Audit and reproduce

Run the audit with Windows PowerShell 5.1:

```powershell
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File `
  .\tools\source-kit\audit-source-kit.ps1
```

The audit enforces the root allowlist, exact manifest file sets and hashes,
text-only patch targets, source-only extensions, script and JSON parsing, and
checks for binary signatures, game assets, secrets, private workstation paths,
hidden control characters, and unwanted authorship markers.

Next assemble into a brand-new path from a fresh clean baseline. Compare the
assembled patch targets, overlay, and `vendor/librw` against the reviewed source
of truth. Then run a clean Release build with `BUILD_PC.bat`.

The kit's `.gitattributes` disables line-ending conversion because the manifests
hash exact file bytes. Verify a fresh clone even when `core.autocrlf=true`; do
not fix a clone failure by regenerating hashes after checkout.

Before a player release, complete the regression matrix in
[VR_PERFORMANCE.md](docs/VR_PERFORMANCE.md). Static inspection, a successful
build, or one fast frame is not a headset performance acceptance test.

## 4. Package

Package only files permitted by the audit. Review third-party notices and every
external download URL immediately before publication. Do not include a built
game executable, runtime DLLs, model packs, game files, or locally downloaded
archives in the source release.

For the separate PCVR player package, include `CUTSCENE_MODE.bat` beside
`reVC.exe` and `docs/CUTSCENE_MODE.md` with the player documentation. The BAT is
self-contained and can also be distributed as a small support download without
replacing the game executable or a player's settings. Keep cutscene mode opt-in;
the log's successful frame submission does not detect a black headset image.

## 5. Publish from clean history


The release repository history must contain only audited patch-kit files. A
working-tree conversion does not remove complete upstream snapshots or binaries
from older commits. Create a fresh history root for the audited tree, verify a
fresh clone with the audit and assembly tests, and only then publish it. Never
push this kit as a normal descendant of a full-tree or binary-bearing history.
