# Contributing and reporting defects

This is an alpha port with public sources and a portable Windows package. Start with the supported XBE and a clean local build. Do not overwrite the bundled SDK with SDK Sync.

## Bug reports

Include:

- Source revision and XBE SHA-1 (the hash only).
- Windows version, CPU, RAM, GPU and driver version.
- Build-tool versions and build duration for compilation problems.
- Arena, characters, mode, action and clear reproduction steps.
- Relevant settings, installed mods and whether the issue persists without them.
- Expected/actual behavior and a sanitized `logs/marvelnemesis.log` copied after closing the game, before launching again.

Remove usernames, private paths, IP addresses and other personal data from logs. Do not paste a whole raw log without reading it first. Public reports must not contain disc images, XBE/EXE/PDB, original/generated game code, saves, texture/model/audio/video resources, XPR bundles, memory dumps or full F3 capture packages. A log line or image does not alone establish the technical cause.

## Source contributions

Keep changes focused, preserve existing third-party attribution and describe what you tested and what remains untested. New rendering or input changes should retain the existing fallback and error handling. Do not add game data to fixtures or examples. Do not copy implementation code from a project with an incompatible license into the SDK.

Before committing, inspect both `git status` and `git diff --cached --name-only`: ignore rules do not prevent `git add -f`. Builds, game data, logs, saves, mods and private research must remain outside the tracked tree. A source contribution must not include generated code or built executables. The Windows release is produced separately through the packaging allowlist; never upload the prepared game-code DLL or `compiler/work/`. The six documentation captures I supplied in `docs/screenshots/` are documentation illustrations, not an exception for extracted game assets; see [Showcase](docs/SHOWCASE.md).

Do not bypass the executable hash gate to claim support for another revision. Such support requires a deliberate mapping and validation of all affected hooks.
