# Building from your own game files

These instructions are for developers. Players can use the portable Windows ZIP from [Releases](https://github.com/15Cyber/marvel-nemesis-recomp/releases), supply their own game in `assets/`, and run the EXE. The included compiler prepares game code on first launch without a Visual Studio installation.

## Requirements

- 64-bit Windows and a Direct3D 12-capable GPU. Existing gameplay validation is on Windows 11/NVIDIA; Windows 10 and AMD/Intel coverage remain unconfirmed.
- A CPU supporting the x86-64-v2 instruction set selected in `CMakePresets.json`.
- Visual Studio with **Desktop development with C++**, Windows SDK, **C++ Clang tools for Windows** and **C++ CMake tools for Windows** (including Ninja).
- Git available in PATH and Internet access to fetch Zydis, toml++, Dear ImGui and SDL3.
- CMake 3.25 or newer. The project compiles as C++20 with Clang and lld; this is not an MSVC-cl build configuration.

Recorded development environment, read from existing build metadata/file versions: **Visual Studio 18.11.12306.390**, **standalone LLVM/Clang 23.1.3**, **CMake 4.3.1-msvc1**. These are the observed versions, not proven minimums. Ninja's exact version and clean-build duration have not been recorded. Visual Studio's bundled Clang is also installed, but `build.bat` prefers a standalone installation when present.

The build uses both standalone Clang and Visual Studio's LLVM utilities. On 19 October 2026, I successfully compiled the public candidate in its fresh folder with Clang 23.1.3, CMake 4.3.1-msvc1 and Windows SDK 10.0.26100.0. This validates that local build path; it does not establish a second-machine result. See [Validation](VALIDATION.md).

## 1. Prepare the source

Obtain the complete source tree, including the bundled `sdk/`. Use a fresh local folder; do not copy an existing `out`, `generated`, settings, saves or mods folder into it. Do not run SDK Sync.

The source tree contains no original executable, generated game code or prepared game-code DLL. The Windows release provides a separately built host EXE and portable compiler tools.

## 2. Extract your own disc image

If you already have an extracted original Xbox game directory, copy its contents into `assets/`. For an image of your own disc, an XDVDFS-capable tool such as [xdvdfs](https://github.com/antangelo/xdvdfs) can extract it. Consult that tool's official documentation and run `xdvdfs unpack -h` for the arguments supported by your installed version. Extract locally, then copy the resulting game files; no game download is provided here.

Expected layout:

```text
assets/
  default.xbe
  Bundles/
  Materials/
  Models/
  ...the remaining game directories...
build.bat
sdk/
```

Keep the complete game data and directory layout. An ISO alone is not sufficient as build input: `assets/default.xbe` must be present. Runtime ISO loading exists separately; its mods-overlay path does not yet have individual validation.

In PowerShell, from the source folder:

```powershell
Get-FileHash -LiteralPath .\assets\default.xbe -Algorithm SHA1
```

Required SHA-1: **`66481feb81d9a00a63f0d2f0a203a297b712cfb5`**. Do not bypass the check or substitute addresses for another image. It protects fixed-address hooks; the runtime also verifies the game it loads.

## 3. Build

Run `build.bat` from Explorer or a terminal. It checks the XBE before compilation, locates Visual Studio, configures CMake and builds the game. If optional components are missing it offers to install them through Visual Studio Installer; installing prerequisites manually first avoids that prompt.

The first configuration downloads dependencies from their upstream repositories. The first build generates C++ from your XBE and compiles it. Time and resource requirements depend on the PC; no 30–90 minute guarantee is made.

Output: `out/build/release/MarvelNemesis.exe` and `MarvelNemesis.dll`. The EXE contains the host runtime and title-specific hooks; generated game code lives in the separate DLL. Retain the source folder's `assets` for the usual development layout. Accept the launch prompt or start the executable yourself.

## 4. First-run checklist

- Start with no mods and default settings.
- Open the original Options and PC Settings; enter a known arena and return to the menu.
- Confirm keyboard/controller assignment, sound and focus pause.
- Save manually, close the process, reopen and load the save.
- Record hardware, tool versions, build duration and any errors before enabling optional effects/mods.

Preserve your existing saves separately before testing a new build. Validation results and their limits are recorded in [Validation](VALIDATION.md).

## Distribution

Run `release.bat`, or `release.ps1` with explicit `-BuildDirectory` and `-OutputDirectory`, after building the host target. The script reads an explicit file allowlist and creates `MarvelNemesis-Windows-x64.zip` plus a SHA-256 digest. Choose a fresh output directory for each package; existing ZIPs are not overwritten.

It packages the host EXE, Clang/LLD, recompiler, import library, headers/configuration, required Windows runtime DLLs, licenses and instructions. It excludes the prepared `MarvelNemesis.dll`, `compiler/work/`, original XBE/data, generated sources, PDB, saves and mods. The `.lib` included with the compiler is an import library describing the host's exported interfaces, not game instruction bodies.

Do not distribute your locally generated game-code DLL or work directory. SDK templates are upstream development material and do not define this project's release policy.

## Distribution checks

From a Visual Studio x64 developer shell with Clang/LLD and Python in PATH:

```powershell
python tests/check_distribution.py --package out/packages/MarvelNemesis-Windows-x64.zip
```

The synthetic DLL fixture checks native overrides, fallback bodies, direct original-body calls, runtime imports and exceptions across the DLL boundary. Package checks verify the allowlist and hashes. `MarvelNemesis.exe --prepare-only` performs preparation/loading and exits without starting gameplay; this is a diagnostic path for isolated first-run checks.
