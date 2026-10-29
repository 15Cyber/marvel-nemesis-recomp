# Alpha 0.1.0-alpha.2 — portable Windows package

## Play without installing Visual Studio

Download **MarvelNemesis-Windows-x64.zip**, extract the entire ZIP, put your own supported Xbox game in `assets/`, and run `MarvelNemesis.exe`.

The package includes the host runtime, portable recompiler, Clang/LLD and required runtime DLLs. First launch prepares your game's code locally behind a progress window, then starts the game automatically. Later launches reuse `MarvelNemesis.dll`. No Visual Studio, CMake or Ninja installation is required for this player package.

**No original game data, XBE, ISO, generated game sources or prepared game-code DLL is distributed.** Bring your own legally acquired copy. Required XBE SHA-1: `66481feb81d9a00a63f0d2f0a203a297b712cfb5`. Other revisions remain rejected.

## What changed

- Split generated game code from the host EXE into a locally prepared DLL.
- Added automatic first-launch translation/compilation, progress, logs and cached reopening.
- Retain completed compilation files after interruption; reuse them on the next launch.
- Build a missing DLL again using completed objects; report missing compiler files.
- Added allowlisted portable packaging, SHA-256 hashes and compiler/runtime license notices.
- Fixed configuration loading/preparation in installation paths containing accented characters.
- Retain the existing PC Settings, graphics options, controls, cheats, Development selector, portable saves and experimental mods.

First-launch preparation is adapted from TGP482/SCDA-XBERecomp V2 (MIT), while preserving this port's local SDK compatibility fixes. Built with TGP482/XboxXBERecomp.

## Validation and alpha limits

Synthetic EXE/DLL tests, package inventory/hashes, isolated first/cached preparation, interruption/resume, missing-tool diagnostics, missing-DLL recovery and native boot initialization were checked on the existing Windows/NVIDIA PC. See [Validation](VALIDATION.md) for exact scope and measured preparation examples. Preparation time depends on hardware.

A second PC without development tools installed and a full hands-on gameplay/save regression of this new package remain pending. This alpha is intended for community testing. Preserve saves separately and extract updates into a fresh folder. Keep `compiler/` for repair and updates.

[Building](BUILDING.md) remains available for developers. [Known issues](KNOWN_ISSUES.md) and [Usage](USAGE.md) describe current limits. Do not attach game files, prepared DLLs, `compiler/work/`, saves or memory dumps to reports.

[Watch the showcase video](https://youtu.be/LgDM9how-TY). I am 15Cyber; I developed this with OpenAI Codex and Anthropic Claude AI assistance. Unofficial fan project; no affiliation with Marvel, EA, Microsoft or the original developers.
