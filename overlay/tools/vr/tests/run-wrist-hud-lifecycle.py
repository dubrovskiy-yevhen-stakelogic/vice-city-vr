#!/usr/bin/env python3
"""Compile the production wrist HUD with host adapters; no renderer or headset.

Run from a Visual Studio developer shell with --compiler cl, or use clang++/g++.
An optional --source-root points to another assembled src/tools root for baseline
regression checks. Output must be a new directory; source files are never changed.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def function(source, name):
    match = re.search(r"(?m)^[\w \t:*&]+\b" + re.escape(name) + r"\([^;{}]*\)\s*\{", source)
    if not match:
        raise RuntimeError(f"Function not found: {name}")
    start = source.index("{", match.start())
    depth = 1
    end = start + 1
    while depth and end < len(source):
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    if depth:
        raise RuntimeError(f"Unclosed function: {name}")
    return source[match.start():end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--compiler", default="cl" if shutil.which("cl") else "c++")
    parser.add_argument("--out-dir", type=Path)
    args = parser.parse_args()
    source_dir = args.source_root.resolve() / "src" / "vr"
    source = (source_dir / "OpenXRVR.cpp").read_text(encoding="utf-8-sig")
    settings = (source_dir / "WristHudSettings.h").read_text(encoding="utf-8-sig")
    hud_path = args.source_root.resolve() / "src" / "render" / "Hud.cpp"
    if hud_path.exists():
        hud_source = hud_path.read_text(encoding="utf-8-sig")
    else:
        patch = (args.source_root.resolve().parent / "patches" / "vice-city-vr.patch").read_text(encoding="utf-8-sig")
        hud_source = "\n".join(line[1:] for line in patch.splitlines() if line.startswith("+") and not line.startswith("+++"))
    panel_rect = function(hud_source, "GetVrWristHudPanelRect").replace(
        "GetVrWristHudPanelRect(", "ProductionWristHudPanelRect(", 1)
    hand = function(source, "GetTrackedHandMatrix")
    guard = re.search(r"if\(!handMatrix[\s\S]*?return false;", hand)
    if not guard or "!gFramePrepared" not in guard.group():
        raise RuntimeError("Production hand lifetime guard changed; review the adapter")
    if "GetTrackedHandMatrix(hand, handMatrix, grip, trigger)" not in function(source, "GetTrackedVisualHandMatrix"):
        raise RuntimeError("Visual-hand delegation changed; review the adapter")
    submit = function(source, "SubmitStereoFrame")
    hud_call = re.search(r"(?m)^\tconst bool showHud=UpdateHudSwapchain[^;]*;", submit)
    if not hud_call:
        raise RuntimeError("HUD submit call changed")
    restore = submit.rfind("\tRestoreCamera(camera);", 0, hud_call.start())
    capture = submit.find("\tWristHudFrame wristFrame;")
    legacy = capture < 0
    if restore < 0 or (not legacy and not capture < restore):
        raise RuntimeError("HUD anchors must be captured before camera restore")
    start = restore if legacy else capture
    ordering = submit[start:hud_call.end()]
    if not legacy and "CaptureWristHudFrame(&wristFrame);" not in ordering[:ordering.index("RestoreCamera(camera);")]:
        raise RuntimeError("Missing pre-restore anchor capture")
    if re.search(r"gFramePrepared\s*=", ordering):
        raise RuntimeError("HUD submission must not resurrect the prepared-frame flag")
    fields_start = settings.index("struct WristHudSettings")
    fields_end = settings.index("\n\tstatic int Bound", fields_start)
    if args.out_dir:
        output = args.out_dir.resolve()
        output.mkdir(parents=True, exist_ok=False)
    else:
        output = Path(tempfile.mkdtemp(prefix="vcvr-wrist-lifecycle-"))
    (output / "wrist-hud-settings-extracted.inc").write_text(settings[fields_start:fields_end] + "\n};\n", encoding="utf-8")
    (output / "wrist-hud-rect-extracted.inc").write_text(panel_rect, encoding="utf-8")
    tracking = "\n\n".join(function(source, name) for name in ("ToGameVector", "ToTrackingVector", "ToTrackingPosition", "Rotate"))
    tracking += "\nstatic bool GetTrackedVisualHandMatrix(int hand, CMatrix *handMatrix, float *, float *)\n{\n"
    tracking += "\t++handCalls;\n\t" + guard.group() + "\n\t*handMatrix = handMatrices[hand];\n\treturn true;\n}\n"
    (output / "wrist-hud-tracking-extracted.inc").write_text(tracking, encoding="utf-8")
    production = '#include "' + (source_dir / "OpenXRWristHud.h").as_posix() + '"\n'
    production += function(source, "RestoreCamera") + "\n" + function(source, "UpdateHudSwapchain")
    production += "\nstatic bool SubmitHudForTest(RwCamera *camera)\n{\n" + ordering + "\n\treturn showHud;\n}\n"
    if legacy:
        production = "#define WRIST_HUD_LEGACY\n" + production
    (output / "wrist-hud-production-extracted.inc").write_text(production, encoding="utf-8")
    fixture = Path(__file__).resolve().with_name("wrist-hud-lifecycle-test.cpp")
    compiler = args.compiler
    msvc = Path(compiler).stem.lower() in ("cl", "clang-cl")
    executable = output / ("wrist-hud-lifecycle-test.exe" if msvc else "wrist-hud-lifecycle-test")
    if msvc:
        command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX", "/O2", "/I" + str(output),
                   "/Fe" + str(executable), "/Fo" + str(output / "wrist-hud-lifecycle-test.obj"), str(fixture)]
    else:
        command = [compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                   "-I", str(output), str(fixture), "-o", str(executable)]
    print("Extracted production source:", source_dir, flush=True)
    print("Test artifacts:", output, flush=True)
    subprocess.run(command, check=True, cwd=output)
    subprocess.run([str(executable)], check=True, cwd=output)


if __name__ == "__main__":
    main()
