# Alpha 0.1.0 — first public source release

Marvel Nemesis: Rise of the Imperfects on Windows through static recompilation of the original Xbox version, built with [TGP482/XboxXBERecomp](https://github.com/TGP482/XboxXBERecomp).

## Highlights

- Native English PC Settings menu, synchronized with the F10 panel.
- Windowed, borderless and fullscreen display options; 20–200% render scale, frame-rate controls and V-Sync.
- MSAA, SMAA, anisotropic filtering, optional SSAO/GTAO and bloom, plus character-shadow resolution/softness controls.
- Keyboard and controller remapping, device assignment and local two-player Versus.
- Focus pause including audio, intro skipping and portable saves.
- Optional per-player health/energy cheats and content unlocks.
- Development mission selector and access to three retained experimental test scenes, with autosave suppressed during Development sessions.
- Experimental file-replacement mods; a manually adapted community texture was tested, but is not included.

## Build your own copy

**Sources only. No game executable, disc image, generated game code, original game resources or texture packs are supplied.** Provide files from your own legally acquired original Xbox copy and follow [Building](BUILDING.md). We do not support piracy.

Supported `default.xbe` SHA-1: **`66481feb81d9a00a63f0d2f0a203a297b712cfb5`**. Other revisions and platforms are not supported build inputs. Keep the bundled SDK.

## Alpha coverage and limitations

The maintainer validated a fresh-folder build, gameplay smoke checks and manual save/close/reopen/load on one Windows/NVIDIA PC. Campaign and hardware coverage remain incomplete; AMD/Intel GPUs and other PCs need community testing. Rendering currently uses Direct3D 12. Vulkan, HDR, additional character slots and new network/co-op modes are not implemented.

Developer scenes can contain unfinished behavior. Development sessions do not roll back progress in memory, and manual saves remain possible: preserve your normal campaign save before exploration. Mods require compatible Xbox paths/formats and remain subject to guest-memory limits.

See [Known issues](KNOWN_ISSUES.md), [Validation](VALIDATION.md) and [Usage](USAGE.md). Report reproducible defects with hardware, settings and a sanitized log; do not attach game files, executables, saves, texture packs or memory dumps.

## Showcase and credits

[Watch menus and gameplay on YouTube](https://youtu.be/LgDM9how-TY).

Maintained by **15Cyber**, with AI assistance from **OpenAI Codex** and **Anthropic Claude**. Project-owned additions use MIT; existing third-party notices are preserved. This is an unofficial fan project, not affiliated with Marvel, Electronic Arts, Microsoft or the original developers.
