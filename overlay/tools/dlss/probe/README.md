# DLSS-NR regression probes

## Game reconstruction chain

`nr-chain-probe.vcxproj` compiles the actual `src/vr/DLAA.cpp`. A small
headless host supplies its D3D12 device, command list, descriptor heap and
resource retirement services. It runs `BeginFrame` and `EvaluateEye` for both
eyes with synthetic color/depth, including the game's motion-generation,
NR and DLSS SR/DLAA reconstruction code. No game window or OpenXR session
is created.

```powershell
MSBuild.exe tools\dlss\probe\nr-chain-probe.vcxproj /m /p:Configuration=Release /p:Platform=x64 /p:StreamlineSdk=D:\SDKs\Streamline
```

Pass your external Streamline SDK directory with `StreamlineSdk`; no SDK
payload belongs in the public source kit. Omitting that property retains the
development-checkout fallback at `vendor/streamline`.

Copy the resulting `nr-chain-probe.exe` beside the user-supplied runtime
DLLs. Run it from a separate evidence directory, because `DLAA.cpp` writes
`streamline_dlaa.log` in the working directory. Arguments are DLSS mode
(0 Full, 1 Quality, 2 Balanced, 3 Performance), NR pass count, output width,
and output height:

```powershell
& 'C:\path\to\VR-runtime\nr-chain-probe.exe' 1 2 3612 3976
```

Each run checks baseline -> NR -> baseline -> NR, including creation/warmup
frames. Both eyes must finish reconstruction at the requested output size.
After GPU readback, final NR RGB hashes must differ from baseline, and the
restored baseline must match its original hash. Success ends with
`CHAIN_COMPLETE failures=0` and a nonzero active-frame count.

This test reproduces Streamline error 27 in the previous game code: it sent
constants twice for one frame/viewport, changing `reset` when NR became the
DLSS input. The repaired path submits DLSS constants once, after choosing
that input, and reserves separate viewports for Streamline NR histories.

The test does not submit to a headset or measure in-game performance.

## Color subrectangle contract

This headless D3D12 probe uses the user-supplied NR model, the installed
caller forwarder, and Streamline from the specified runtime directory. It
does not create a window, start OpenXR, or launch the game.

Build from the repository root with Visual Studio 2022:

```powershell
MSBuild.exe tools\dlss\probe\nr-subrect-probe.vcxproj /m /p:Configuration=Release /p:Platform=x64 /p:StreamlineSdk=D:\SDKs\Streamline
```

Run with the runtime directory, per-eye work width/height, and optionally
the number of sequential passes (1 through 3):

```powershell
& .\tools\dlss\probe\bin\Release\nr-subrect-probe.exe 'C:\path\to\VR-runtime' 2400 2640 3
```

On the tested Ada runtime, `wide-right` is a negative control: a nonzero
color subrect X origin returns NGX `0xBAD00005` (`InvalidParameter`) and
leaves the output empty. A nonzero depth X origin succeeds with local color.
`staged-right` uses the same `CopyNeuralColorInput` helper as the game to
copy the right eye into a local texture before evaluating at color X=0.

The probe reads back GPU output and checks that successful evaluations
write nonzero RGB and change the synthetic source pattern. With 2 or 3
passes, it additionally checks separate left/right handles and their
temporal histories over three frames. An unexpected result produces a
nonzero exit code; success ends with `PROBE_COMPLETE failures=0`.

After GPU completion and releasing all NR handles, the probe terminates
its own process without calling `slShutdown`. The direct model and
Streamline share the driver NGX core; teardown of that mixed ownership
hung in the initial standalone probe. This is confined to the test process.

These checks isolate the NR resource contract. They do not measure game
frame time, validate motion vectors, or establish headset visual quality.
