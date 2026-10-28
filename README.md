# Dirt1_Core_Bloom_Fix

A `dinput8.dll` proxy for **Colin McRae: DiRT** (2007, EGO engine 1.0) that makes the game run
on modern multi-core CPUs and Windows 10/11, and removes the yellow bloom without touching any
game files.

- No more crashes at startup or on race start
- No hand-editing of `hardware_settings_restrictions.xml`, `hardware_settings_config.xml` or
  `workerMap*.xml`, and no `start /affinity` shortcuts
- Engine threads are placed on real physical cores (P-cores, no HT siblings / E-cores)
- Bloom, the orange tint and the vignette are adjusted on the fly, configurable in an `.ini`
  (similar to [Colin McRae DIRT - Graphics Mod](https://www.moddb.com/mods/colin-mcrae-dirt-graphics-mod),
  but the mod itself isn't needed)

Targets `DiRT.exe` **1.2** (patch 1.22, no StarForce). Other versions get the generic fixes; the
main-thread tweak is applied only to 1.2.

## Installation

1. Build it (see below) or take `dinput8.dll` from a release.
2. Copy `dinput8.dll` and `Dirt1_Core_Bloom_Fix.ini` into the game folder, next to `DiRT.exe`.
3. Undo old workarounds: restore the original `workerMap*.xml` / `hardware_settings_restrictions.xml`,
   remove `/affinity` from shortcuts, turn off compatibility mode.
4. Start the game. `Dirt1_Core_Bloom_Fix.log` shows the detected CPU, the chosen cores, the display
   modes and every rewritten file.

To uninstall, delete `dinput8.dll`.

## Configuration

Everything lives in `Dirt1_Core_Bloom_Fix.ini`. The defaults are fine for most systems.

| Section | Key | Default | Meaning |
|---|---|---|---|
| `Threads` | `Affinity` | `1` | `1` pin engine threads to the best physical cores, `0` no pinning, `2` original behaviour |
| | `WorkerMap` | `1` | Redirect every `workerMap*.xml` to the game's 4/2/1 core map |
| | `MainThreadSoftAffinity` | `1` | Main thread gets an ideal processor only (like DiRT 2) |
| `PostProcess` | `Enable` | `1` | Rewrite `effects.xml` files on load |
| | `Bloom` | `0.30` | In-race bloom (`bloomAmount`), game uses ~0.8-1.0, `0` = off |
| | `Tint` | `0` | `0` removes the orange/yellow tone (`tintColour` -> white), `keep` = game |
| | `Vignette` | `0` | Dark screen edges |
| | `Brightness` | `0` | Engine parameter `brighness` (sic), game uses about -0.06 |
| | `Contrast`, `Saturation`, `Gamma`, `MotionBlur` | `keep` | Optional overrides |
| | `MenuBloom`, `MenuTint`, `MenuVignette` | `0` | Front end effects |
| `PostProcessOverride` | *any `<Param>` name* | | Free-form overrides for track effects, e.g. `targetLuminance=0.64` |
| `Video` | `MaxVideoMemoryMB` | `1024` | Video memory reported to the game |
| | `FilterModes` | `1` | Filter the D3D9 display mode list |
| | `MaxResolutions` | `12` | Max distinct resolutions (largest + desktop) |
| | `MinWidth` | `800` | Smaller resolutions are dropped |
| `Misc` | `DPIAware` | `1` | Disable DPI scaling |
| | `Log` | `1` | Write the log file |

Any `PostProcess` value can be `keep` to leave the game's value untouched.

## What was wrong (analysis of DiRT.exe 1.2)

### Startup crash: too many display modes

`0xC0000005` at `DiRT.exe+0x1999BE`. Every D3D9 display mode is written into
`hardware_settings_info.xml` (a `resolution` node plus `refreshRate` children). That XML lives in the
fixed-size `HARDWARE_SETTINGS` pool from `system/applicationMemoryMap.xml`. Modern Windows with a
high refresh rate monitor and DSR/VSR reports dozens of modes, the pool runs out, the allocator
returns `NULL`, and the game crashes while copying the resolution list in the code that matches the
config resolution (`0x748280`).

**Fix:** `IDirect3D9::GetAdapterModeCount/EnumAdapterModes` return a filtered list: at most 12
resolutions (always including the desktop one), each with 60 Hz and the desktop refresh rate.

### Threading in EGO 1.0

- The CPU count is `GetSystemInfo().dwNumberOfProcessors`, i.e. all logical CPUs, ignoring the
  process affinity mask.
- Every thread from `workerMap*.xml` (`core="N"`) and the main thread (`applicationProcessor`) gets a
  **hard** `SetThreadAffinityMask(1 << (N % CPUs))`. `N` is just a logical CPU index, so
  `abovenormal` spin-waiting workers land on HT siblings of the same core, and on E-cores on hybrid
  Intel CPUs. That is the stutter PCGamingWiki works around with a 7-thread affinity mask.
- `hardware_settings_info.xml` gets `cores` = logical CPUs, halved for any Intel CPU with the CPUID
  HTT bit (set on every modern Intel CPU, HT or not). `hardware_settings_restrictions.xml` uses that
  number to pick `workerMap.xml` / `2Core` / `4Core` / `8Core` and stores the result in
  `hardware_settings_config.xml`. It is recalculated when hardware changes, so manual edits keep
  getting lost.
- **`workerMap8Core.xml` is broken twice:**
  - its `view_manager` group references `view_manager_2` and `view_manager_3`, which are not defined
    as `<workerThread>`;
  - it is the only map that puts workers into the physics `solver` and `cloth` groups. The solver
    has no per-thread context for a second worker: the job with thread index 1 gets `NULL` and the
    game crashes on race start (`0xC0000005` at `DiRT.exe+0x562470`, reading `NULL+0x2C` in a worker
    thread). This is the "replace 8Core with 4Core" fix from PCGamingWiki.

### What DiRT 2 (EGO 1.5) does differently, ported here

- CPU topology detection over the **process affinity mask**, separating physical cores from HT
  siblings (DiRT 2 reads APIC IDs via CPUID; here `GetLogicalProcessorInformationEx`, which also
  tells P-cores from E-cores).
- A table mapping the engine's "core N" to a real logical CPU instead of `1 << N`. DiRT 2 on Win7+
  pairs workers on HT siblings; here distinct physical P-cores are filled first, because the maps
  assume one high priority worker per core.
- The main thread only gets `SetThreadIdealProcessor`, no hard affinity.

### What the fix does

| Hook (`DiRT.exe` import table) | Action |
|---|---|
| `GetSystemInfo` | Reports 4 CPUs (2/1 on small CPUs) |
| `SetThreadAffinityMask` | Engine core N -> N-th physical P-core; the main thread is not pinned |
| `SetThreadIdealProcessor` | Same mapping |
| `CreateFileA/W`, `GetFileAttributesExA`, `FindFirstFileA` | `workerMap*.xml` -> the game's `workerMap4Core.xml` (2Core / `workerMap.xml` on small CPUs); `effects.xml` -> rewritten copy in `%TEMP%\Dirt1_Core_Bloom_Fix` |
| `GetProcAddress("DirectDrawCreateEx")` | `IDirectDraw7::GetAvailableVidMem` clamped to 1024 MB |
| `Direct3DCreate9` | `GetAdapterModeCount/EnumAdapterModes` return the filtered mode list |

Plus `SetProcessDPIAware`.

### Post processing without the mod

The "yellow" look comes from `tintColour` (orange, e.g. `225, 143, 100`) + `tintAmount` and a strong
`bloomAmount` in `tracks/<track>/effects.xml`. The Graphics Mod edits 70 of these files by hand.
This fix does it when the game opens a file: the original is read, the configured `<Param>` values are
replaced inside `<Effect chain="Type2">` blocks, and the game gets the copy. Track rules only touch
`tracks/`, menu rules only touch `FE_*` (`chain="Type5"`) effects in `postprocess/effects.xml`. That
file also holds in-game effects such as the red damage flash, which are left alone.

### Other modern OS issues

- **Video memory** is read with `IDirectDraw7::GetAvailableVidMem` (DWORD) and divided as a signed
  int, so 2+ GB cards turn negative. Fixed by the clamp.
- **Windows version:** the game only recognises XP (5.1) and Vista (6.0). The flags are unused in
  1.2, so compatibility mode isn't needed.
- **Thread pool per logical CPU:** one pool creates a thread for every logical CPU (dozens on
  24-32 thread CPUs). With the fix it sees 4.
- **No Large Address Aware flag:** the game has 2 GB of address space. DiRT 2 is the same and runs
  fine, so it's optional: if crashes in the middle of a race remain, set the LAA flag on `DiRT.exe`
  with any 4GB patch tool (keep a backup).
- **Timer** (QueryPerformanceCounter) uses 64-bit math, no issues.
- **Audio** is OpenAL 1.1 (2007 Creative router). If sound misbehaves, install the official OpenAL
  runtime (`oalinst.exe`) or OpenAL Soft.
- **StarForce** is only in 1.0-1.1 and was removed in 1.2. Online (DemonWare) is gone, LAN works.

## Building

**Visual Studio 2022:** open `Dirt1_Core_Bloom_Fix.sln`, configuration `Release|x86`. Release uses
the static CRT, no redistributable needed. Set `GameInstallPath` in the `.vcxproj` to copy the DLL
into the game folder after each build.

**llvm-mingw** (Linux/Windows):

```sh
./build.sh          # uses i686-w64-mingw32-clang++ from PATH, output in build/
```

The llvm-mingw build links against UCRT, which ships with Windows 10/11.

## Credits

- Bloom/tint analysis is based on the parameter changes of
  [Colin McRae DIRT - Graphics Mod](https://www.moddb.com/mods/colin-mcrae-dirt-graphics-mod) by NoobSolid26.
- Known workarounds collected on [PCGamingWiki](https://www.pcgamingwiki.com/wiki/Colin_McRae:_DiRT).

## License

MIT, see [LICENSE](LICENSE).
