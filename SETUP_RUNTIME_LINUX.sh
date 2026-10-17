#!/bin/sh
# Vice City VR - Required Runtime Setup for Linux (Steam Play / Proton).
# Linux counterpart of SETUP_RUNTIME.bat. Needs only python3.
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required. Install it with your distribution's package manager."
    exit 1
fi
if [ -f "$here/reVC.exe" ]; then
    python3 -I "$here/tools/dlss/install-runtime-linux.py" --game-dir "$here" "$@"
else
    python3 -I "$here/tools/dlss/install-runtime-linux.py" "$@"
fi
status=$?
echo
if [ "$status" -ne 0 ]; then
    echo "Setup did not complete. Read the message above before starting the game."
fi
exit "$status"
