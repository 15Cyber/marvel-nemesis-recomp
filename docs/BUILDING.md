# Building from your own game files

## Requirements

- 64-bit Windows and a Direct3D 12-capable GPU. Existing gameplay validation is on Windows 11/NVIDIA; Windows 10 and AMD/Intel coverage remain unconfirmed.
- A CPU supporting the x86-64-v2 instruction set selected in `CMakePresets.json`.
- Visual Studio with **Desktop development with C++**, Windows SDK, **C++ Clang tools for Windows** and **C++ CMake tools for Windows** (including Ninja).
- Git available in PATH and Internet access to fetch Zydis, toml++, Dear ImGui and SDL3.
- CMake 3.25 or newer. The project compiles as C++20 with Clang and lld; this is not an MSVC-cl build configuration.

Recorded development environment, read from existing build metadata/file versions: **Visual Studio 18.11.12306.390**, **standalone LLVM/Clang 23.1.3**, **CMake 4.3.1-msvc1**. These are the observed versions, not proven minimums. Ninja's exact version and clean-build duration have not been recorded. Visual Studio's bundled Clang is also installed, but `build.bat` prefers a standalone installation when present.

The build uses both standalone Clang and Visual Studio's LLVM utilities. On 19 October 2026, the maintainer successfully compiled the public candidate in its fresh folder with Clang 23.1.3, CMake 4.3.1-msvc1 and Windows SDK 10.0.26100.0. This validates that local build path; it does not establish a second-machine result. See [Validation](VALIDATION.md).

## 1. Prepare the source

Obtain the complete source tree, including the bundled `sdk/`. Use a fresh local folder; do not copy an existing `out`, `generated`, settings, saves or mods folder into it. Do not run SDK Sync.

The alpha is distributed as sources only. No original executable, generated code or prebuilt game executable is provided.

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

Output: `out/build/release/MarvelNemesis.exe`. Retain the source folder's `assets` for the usual development layout. Accept the launch prompt or start the executable yourself.

## 4. First-run checklist

- Start with no mods and default settings.
- Open the original Options and PC Settings; enter a known arena and return to the menu.
- Confirm keyboard/controller assignment, sound and focus pause.
- Save manually, close the process, reopen and load the save.
- Record hardware, tool versions, build duration and any errors before enabling optional effects/mods.

This is a manual checklist for the tester; no game or compiler was run as part of preparing these sources. Preserve your existing saves separately before testing a new build.

## Distribution

Do not distribute the built game, PDB, generated sources, assets or community mods with this alpha. The original development binary-packaging script is omitted from the project root. SDK templates are upstream development material and do not define this project's release policy.
