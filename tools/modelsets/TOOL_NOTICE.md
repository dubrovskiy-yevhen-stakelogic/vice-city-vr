# Modern model-set builder notice

These utilities are distributed to let players assemble an optional Modern
asset overlay locally from a legally owned Vice City installation and external
packs they obtain themselves.

- `build-modern-modelset.ps1` orchestrates validation and staging.
- The IMG/TXD helper scripts and `txdcompress.cpp` are provided in source form.
- `txdcompress.exe` is the Windows x64 build of the adjacent
  `txdcompress.cpp` source; it imports only Windows `KERNEL32.dll`.

The utilities do not grant rights to the original game data or third-party mod
assets. Generated `modelsets\modern` content is for the player's local,
personal installation and must not be redistributed.

This repository does not currently grant a general license to reuse or
redistribute these utility sources separately from the Vice City VR release.
Third-party components, where present, retain their own terms.
