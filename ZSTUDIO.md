# ZStudio RetroFE

A fork of [CoinOPS-Official/RetroFE](https://github.com/CoinOPS-Official/RetroFE) (branch `current`, from commit `caab32e`) maintained by **The Real Z Studios** for the ArcadeMaster cabinets (racing, flight, lightgun stand-ups). Licensed GPLv3 like upstream (`LICENSE.txt`). Every change is marked in the source with `ArcadeMaster`.

## Changes vs upstream

| Area | Change |
|---|---|
| Stability | Null-item guards after scroll/select in **empty collections** (was an access violation in `RetroFE::run`); `Page` no longer dereferences `playlists.end()` for collections without playlists |
| Diagnostics | Crash log lines are `module+RVA symbol (file:line)` via dbghelp. Ship `retrofe.pdb` next to the exe. |
| Cabinets | No modal crash dialog (it blocked unattended cabinets forever). Opt in with `RETROFE_CRASH_DIALOG=1`. |
| Input | **Touch**: tap / double tap / long press / two-finger tap / swipes / drag-scroll / screen-zone taps. **Mouse wheel.** **Trackball and spinner** (relative axes). Touch-synthesised clicks are ignored so lightgun/mouse bindings never double-fire. Token reference: `controls.conf` section below. |

## Build (Windows, MSVC x64)

Requirements: VS 2022 Build Tools (C++), CMake (bundled with VS), Git, and the **GStreamer 1.24.x MSVC x86_64 runtime + devel** MSIs. They can be unpacked without installing: `msiexec /a <msi> /qn TARGETDIR=<dir>`.

```powershell
git clone --recurse-submodules https://github.com/jlvandusen/zstudio-retrofe.git
# SDL include shim: the source uses <SDL2/SDL.h>, the vendored headers are flat
New-Item -ItemType Directory -Force shim\include\SDL2
Get-ChildItem zstudio-retrofe\RetroFE\ThirdParty -Directory -Filter 'SDL2*' | % { Copy-Item "$($_.FullName)\include\*.h" shim\include\SDL2 }
$shim = (Resolve-Path shim\include).Path; $gst = '<dir>\gstreamer\1.0\msvc_x86_64'
cmake -A x64 -B zstudio-retrofe\RetroFE\Build -S zstudio-retrofe\RetroFE\Source -DGSTREAMER_ROOT=$gst `
  -DSDL2_INCLUDE_DIR=$shim -DSDL2_IMAGE_INCLUDE_DIR=$shim -DSDL2_MIXER_INCLUDE_DIR=$shim -DSDL2_TTF_INCLUDE_DIR=$shim `
  "-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /Zi /DNDEBUG" "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=/DEBUG /OPT:REF /OPT:ICF /INCREMENTAL:NO"
cmake --build zstudio-retrofe\RetroFE\Build --config Release --parallel
```

### Runtime folder: important

The `Package/Environment/Windows/retrofe` submodule ships **GStreamer 1.22.7** DLLs. They don't match the 1.24 headers this builds against, and that mismatch caused crashes. Assemble `retrofe\` from:

- the **matching GStreamer 1.24** `bin\*.dll` + `lib\gstreamer-1.0\*.dll` + `libexec\gstreamer-1.0\gst-plugin-scanner.exe`. The scanner probes plugins out-of-process, so a bad DLL can't kill RetroFE.
- the SDL2 / SDL2_image / SDL2_mixer / SDL2_ttf DLLs from `RetroFE/ThirdParty` (they match the compiled headers).
- `libusb-1.0.dll`, `libserialport.dll`, `libcurl.dll`, `zlib.dll`, `zlib1.dll` from `RetroFE/ThirdParty` (missing from the deps bundle).
- the non-GStreamer files from the deps submodule (fonts etc.).
- **Do not ship** `gstfrei0r.dll` or `PacDrive64.dll` in that folder. RetroFE points `GST_PLUGIN_PATH`/`FREI0R_PATH` at it, so the plugin scan loads them, and both crash.

[ZStudio-ArcadeMaster](https://github.com/jlvandusen/ZStudio-ArcadeMaster) `retrofe/Build-RetroFE.ps1` automates all of the above.

## `controls.conf` input tokens added

`touchTap`, `touchDoubleTap`, `touchLongPress`, `touchTwoFingerTap`, `touchSwipeUp/Down/Left/Right`, `touchDragUp/Down/Left/Right`, `touchTapLeft/Right/Top/Bottom/Center`, `mouseWheelUp/Down/Left/Right`, `mouseAxisX-/X+/Y-/Y+` (aliases `mouseX±`, `mouseY±`, `spinnerLeft/Right`, `trackballUp/Down/Left/Right`).

Tunables: `touchPulseMs` (90), `touchTapMaxMs` (300), `touchDoubleTapMs` (350), `touchLongPressMs` (650), `touchSwipeMaxMs` (450), `touchMoveTolerance` (0.03), `touchSwipeMinDistance` (0.12), `touchDragDeadzone` (0.05), `mouseAxisStep` (30), `mouseAxisHoldMs` (120).

Example (touch kiosk):
```
up = Up, touchDragDown, mouseWheelUp
down = Down, touchDragUp, mouseWheelDown
pageUp = PageUp, touchSwipeDown
pageDown = PageDown, touchSwipeUp
select = Return, touchTap
back = Escape, touchTwoFingerTap
random = R, touchLongPress
```

## Keeping up with upstream

```
git fetch upstream
git merge upstream/current      # resolve conflicts in files marked ArcadeMaster
```
