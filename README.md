# Marvel Nemesis: Rise of the Imperfects — Xbox recomp

**Alpha source candidate. Fresh-folder build, gameplay smoke checks and a save/close/reopen/load cycle were approved on the maintainer's PC. Coverage is limited; see [Validation](docs/VALIDATION.md).**

An unofficial Windows PC port of the original Xbox game, built with [TGP482/XboxXBERecomp](https://github.com/TGP482/XboxXBERecomp). This project is not affiliated with, endorsed by, or sponsored by Marvel, Electronic Arts, Microsoft, or the original developers. Game names and trademarks belong to their respective owners.

## Source-only distribution

This repository provides port/runtime source and build instructions. It does not include the game, executable, generated game code, disc images, textures, audio, saves, or community texture packs. You must supply your own game files and build locally. Do not upload these files to issues or pull requests.

Only an original Xbox `default.xbe` with SHA-1 **`66481feb81d9a00a63f0d2f0a203a297b712cfb5`** is supported. Other revisions, regions, prototypes, and the GameCube/PS2/PSP versions are unsupported. The check identifies the executable, not the completeness of the rest of your game data.

## Build and run

1. Read [Building](docs/BUILDING.md). Install Visual Studio with Desktop development with C++, Clang/LLVM, CMake, Ninja, a Windows SDK and Git.
2. Place your extracted game files in `assets/`, with `default.xbe` directly inside it.
3. Run `build.bat`. Dependencies are fetched on the first configuration; Internet access is required. Total first-build duration has not been measured; the successful local fresh-folder build is recorded in [Validation](docs/VALIDATION.md).
4. The locally built game is `out/build/release/MarvelNemesis.exe`.

Keep the bundled `sdk/`: it contains local compatibility changes. Do not run SDK Sync or substitute an upstream SDK.

## Features

- Native English **Options → PC Settings** menu and synchronized **F10** panel.
- Display mode, monitor/resolution, render scale, FPS limit, V-Sync, brightness and contrast.
- Anisotropic filtering, separate MSAA/SMAA, optional SSAO/GTAO, shadow resolution/softness, and optional bloom.
- Keyboard/controller coexistence, player assignment and remapping; local two-player Versus.
- Optional intro-logo skipping, movie framing controls, and pause on focus loss including audio.
- Session cheats for player health/energy and unlock actions persisted through the normal save flow.
- Experimental Development mission/test-scene selector.
- Portable saves and replacement files loaded from `mods/` beside the executable.

See [Usage](docs/USAGE.md) and [known limitations](docs/KNOWN_ISSUES.md). Vulkan, HDR, new multiplayer/co-op modes and new character slots are not implemented.

## Validation and bug reports

Development testing has been performed on one Windows/NVIDIA system. Campaign missions for Thing, Wolverine and Elektra were visited through Development; this does not establish full campaign completion. Several other arenas/characters and local multiplayer were exercised, but coverage is incomplete. AMD/Intel GPUs and other PCs need testing.

Report reproducible failures using the issue template and [reporting instructions](CONTRIBUTING.md). Include hardware, revision, settings and a sanitized log. **Do not attach game files, disc images, executable/PDB, saves, memory dumps or bundles.**

## Attribution and license

Project-owned additions are licensed under [MIT](LICENSE), following the license used by the SDK and the Splinter Cell: Double Agent reference port. Existing SDK and third-party code retains its own copyright notices and licenses. See [NOTICE](NOTICE), the SDK's [MIT license](sdk/LICENSE), and [third-party license notices](docs/licenses/README.md). These software licenses do not grant rights to original game data or trademarks.
