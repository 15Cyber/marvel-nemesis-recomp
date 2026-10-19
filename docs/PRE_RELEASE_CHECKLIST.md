# Alpha preparation checklist

## Prepared

- [x] Separate source candidate; original development repository/checkpoints preserved.
- [x] Source taken from stable checkpoint, without its Git history or local author identity.
- [x] No usable game assets, generated code, executable/PDB, saves, mods, runtime capture packages or private handoffs copied. Six explicitly allowed maintainer-supplied screenshots illustrate the documentation; see SHOWCASE.md.
- [x] Public README, usage, limitations, reporting template and source-only distribution policy.
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
- [x] Maintainer explicitly authorized publication of this source-only alpha to `15Cyber/marvel-nemesis-recomp`.

Optional broader coverage: another machine, AMD/Intel GPUs, ISO mods, autosave Load/reset, remaining missions/arenas. Report coverage honestly rather than marking unperformed tests as passed.
