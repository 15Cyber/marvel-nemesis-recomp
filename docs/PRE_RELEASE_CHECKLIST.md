# Alpha source and portable-package checklist

## Prepared

- [x] Separate source candidate; original development repository/checkpoints preserved.
- [x] Source taken from stable checkpoint, without its Git history or local author identity.
- [x] Source tree excludes usable game assets, generated code, built binaries, saves, mods and private research. Six explicitly allowed documentation screenshots are described in SHOWCASE.md.
- [x] Public README, usage, limitations, reporting template and separate source/portable-package policies.
- [x] Required XBE hash documented and checked by build script before compilation.
- [x] Bundled SDK required; no fallback download of a different SDK.
- [x] Upstream SDK/graphics attribution retained; dependency license texts collected.
- [x] No measured-build-time promise; observed tool versions recorded with scope limits.

## Before publication

- [x] Public identity configured locally as `15Cyber`, using the GitHub-provided noreply address.
- [x] Maintainer selects the MIT pattern used by the Splinter Cell reference and SDK for project-owned additions; existing notices preserved.
- [x] Fresh local Git history uses the public noreply identity; source inventory and author/committer metadata inspected.
- [x] User built this candidate in its fresh folder with their own game data; tool versions and success recorded in VALIDATION.md. Total build time remains unmeasured; only the Ninja interval is available.
- [x] User approved the tested gameplay and manual save/close/reopen/load cycle in this clean build; runtime logs confirm initialization and normal shutdown. Individual coverage limits are in VALIDATION.md.
- [x] No defect reported in this smoke round; broader unresolved cases remain visible in KNOWN_ISSUES.md.
- [x] Source inventory and source archive checked after documentation updates; private archives are never release packages. Repeat this check if sources change again.
- [x] Maintainer explicitly authorized publishing sources and the portable host/compiler package to `15Cyber/marvel-nemesis-recomp`.

## Portable Windows package

- [x] Game instruction bodies are linked into a separate DLL, generated from the player's XBE.
- [x] Packaging uses an explicit allowlist and hashes; prepared game DLL, work files and original assets are excluded.
- [x] Synthetic EXE/DLL tests cover overrides, fallback/original calls, runtime/math imports and exceptions.
- [x] Isolated first preparation and cached reopening passed with developer paths and include/library variables removed.
- [x] Forced interruption/resume, missing compiler diagnostics and recovery from a missing DLL passed.
- [x] Native boot/rendering smoke check initialized the matching game and D3D12; remaining scope is documented in VALIDATION.md.

Optional broader coverage: another machine, AMD/Intel GPUs, ISO mods, autosave Load/reset, remaining missions/arenas. Report coverage honestly rather than marking unperformed tests as passed.
