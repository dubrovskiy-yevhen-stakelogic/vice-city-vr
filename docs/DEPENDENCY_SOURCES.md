# Redistributed dependency source record

This record applies to the Windows x64 dependency binaries shipped with Vice
City VR v0.5.0-alpha. It records the exact upstream artifacts inspected for the
release. The SHA-256 values below were calculated after downloading directly
from the listed upstream URLs on 2026-08-13. It is a provenance and packaging
record, not legal advice.

## OpenAL Soft 1.21.0

The release file `OpenAL32.dll` is the unmodified official Win64
`soft_oal.dll`, renamed as directed by the upstream binary README.

- DLL SHA-256: `5A42440A18A75CE588659158D74D26AB1850EABD34F3B25ABD969A56D871DB42`
- Official binary archive:
  `https://openal-soft.org/openal-binaries/openal-soft-1.21.0-bin.zip`
- Binary archive SHA-256:
  `043CE64163B268CB3068AF949328ADAEDA38C1116DB793182D49AF1BA3E4ECD3`
- Corresponding upstream source:
  `https://openal-soft.org/openal-releases/openal-soft-1.21.0.tar.bz2`
- Source archive SHA-256:
  `2916B4FC24E23B0271CE0B3468832AD8B6D8441B1830215B28CC4FEE6CC89297`
- Main license: GNU Library General Public License version 2 or later
  (`LGPL-2.0-or-later`)
- Additional notices: the spherical-harmonic transform portion is under the
  BSD 3-Clause License, and the built-in BS2B crossfeed code is under the MIT
  License

## libmpg123 1.26.3

The release file `libmpg123-0.dll` is byte-for-byte identical to the DLL in
the official mpg123 1.26.3 x86-64 Windows archive.

- DLL SHA-256: `931DB8AAA4F7060B8E66262F2BC6A4B3EC77C8FF8F175AA9ABA44CBFE7CC063E`
- Official binary archive:
  `https://www.mpg123.de/download/win64/1.26.3/mpg123-1.26.3-x86-64.zip`
- Binary archive SHA-256:
  `B3570BC371C2DB014F8E41388C68FC8EFEB4C2804288A5533EEFB4BCCEC0E270`
- Corresponding upstream source:
  `https://sourceforge.net/projects/mpg123/files/mpg123/1.26.3/mpg123-1.26.3.tar.bz2/download`
- Source archive SHA-256:
  `30C998785A898F2846DEEFC4D17D6E4683A5A550B7EACF6EA506E30A7A736C6E`
- libmpg123 license: GNU Lesser General Public License version 2.1
  (`LGPL-2.1-only`)
- Additional notice: `src/libmpg123/icy2utf8.c` carries a permissive
  BSD-style notice

The complete upstream mpg123 source distribution is mostly LGPL but also
contains separately licensed files, notably the GPL version 2-only CoreAudio
output module at `src/libout123/modules/coreaudio.c`. That module and the
mpg123 command-line programs are not part of the distributed Windows
`libmpg123-0.dll`. The full upstream `COPYING` file is nevertheless included
with the release source archive and copied into `licenses` so the distinction
is explicit.

## Required release layout

The release archive should contain the following files in addition to the DLLs:

```text
THIRD_PARTY_NOTICES.md
DEPENDENCY_SOURCES.md
licenses/
  OpenAL-Soft-COPYING.txt
  OpenAL-Soft-BSD-3-Clause.txt
  OpenAL-Soft-BS2B-MIT.txt
  mpg123-COPYING.txt
  mpg123-ICY-BSD.txt
  AMD-FidelityFX-FSR2-LICENSE.txt
  Microsoft-D3D12-MIT-LICENSE.txt
  source/
    SHA256SUMS.txt
    openal-soft-1.21.0.tar.bz2
    mpg123-1.26.3.tar.bz2
```

The two source archives are the complete, unmodified upstream releases. Keep
the DLLs separate in the game directory so recipients can replace either one
with a compatible modified build. Do not substitute a source URL alone for the
bundled archives without re-evaluating the applicable distribution terms.
