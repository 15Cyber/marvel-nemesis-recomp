# Alpha 0.1.0-alpha.4 — Survival

## What changed since alpha.3

- Added Survival from the main menu: Solo, local two-player Co-op or Co-op with a CPU ally; Classic and Endless waves, three difficulties, bosses and local records.
- Added QUIT GAME and an original green/blue application icon.
- Improved Survival camera, HUD, controls and combat AI. Regular music now returns after a boss wave.
- Added MUSIC ON/OFF in Options > PC Settings. The pause menu keeps its original layout.

## Play

Download **MarvelNemesis-Windows-x64.zip**, extract it to a new writable folder, put files from your own supported original Xbox copy in `assets/`, then run `MarvelNemesis.exe`. The ZIP includes the prebuilt Windows host EXE, portable recompiler and required runtime files. No Visual Studio, CMake or Ninja installation is needed to play. The first launch prepares your game code locally from your own XBE.

The required `default.xbe` SHA-1 is `66481feb81d9a00a63f0d2f0a203a297b712cfb5`. No ISO, XBE, original game assets, prepared game-code DLL, generated code, saves or mods are distributed. Keep the `compiler/` folder beside the EXE.

## Validation and limits

I tested the host build and gameplay on my Windows PC. A friend tested the portable candidate and told me it was ready to release. I do not have a detailed hardware list or build log from that test. The release ZIP is audited against a file allowlist and SHA-256 manifest. The host EXE is the same one my friend tested; only the package documentation was revised afterward. See [Validation](VALIDATION.md) and [Known issues](KNOWN_ISSUES.md) for the scope of testing.

This is an unofficial alpha PC port. Network multiplayer, native Linux and HDR are not implemented. I own and direct this project, with OpenAI Codex and Anthropic Claude as AI tools used in research, implementation, debugging and documentation. Original game rights remain with their respective owners.
