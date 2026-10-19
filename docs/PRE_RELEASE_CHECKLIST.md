# Alpha preparation checklist

## Prepared

- [x] Separate source candidate; original development repository/checkpoints preserved.
- [x] Source taken from stable checkpoint, without its Git history or local author identity.
- [x] No game assets, generated code, executable/PDB, saves, mods, captures or private handoffs copied.
- [x] Public README, usage, limitations, reporting template and source-only distribution policy.
- [x] Required XBE hash documented and checked by build script before compilation.
- [x] Bundled SDK required; no fallback download of a different SDK.
- [x] Upstream SDK/graphics attribution retained; dependency license texts collected.
- [x] No measured-build-time promise; observed tool versions recorded with scope limits.

## Before publication

- [x] Public identity configured locally as `15Cyber`, using the GitHub-provided noreply address.
- [x] Maintainer selects the MIT pattern used by the Splinter Cell reference and SDK for project-owned additions; existing notices preserved.
- [x] Fresh local Git history uses the public noreply identity; source inventory and author/committer metadata inspected.
- [ ] User builds from this clean candidate with their own extracted game; record tool versions, duration and outcome.
- [ ] User tests launch, menus, gameplay, save/reopen and shutdown in this clean build.
- [ ] Record any difference from development behavior; keep unresolved cases visible in KNOWN_ISSUES.md.
- [ ] Review the exact final source archive/tree after any subsequent changes; private archives are never release packages.
- [ ] Maintainer explicitly approves publication and chooses the public repository/tag. No upload has been authorized by preparing these files.

Optional broader coverage: another machine, AMD/Intel GPUs, ISO mods, autosave Load/reset, remaining missions/arenas. Report coverage honestly rather than marking unperformed tests as passed.
