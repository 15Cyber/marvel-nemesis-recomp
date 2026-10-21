# Public alpha validation

## Ultrawide windowed test — 21 October 2026

The isolated public source tree was built in Release with Clang 23.1.3. The standalone layout/projection/video geometry assertions passed, and an isolated installation loaded the new host EXE and game-code DLL through `--prepare-only` with developer environment variables removed.

Tested host EXE SHA-256: `38c629493f020bf867c20e0e28599461ebc06da9442a6538841e3add9de6732d`.

The maintainer then opened the isolated installation in a **1720×720 window on a 1920×1080 desktop**, approved the menus and combat, and supplied fighter-introduction and combat screenshots. This window has the same pixel aspect ratio as 3440×1440. The scene fills the wider window while the HUD stays inside the centred 16:9 area. Native introduction bars remain confined to that area; the maintainer accepted their appearance and reported no interference during combat.

The inspected runtime session reports the supported XBE verified, `horizontal_scale=0.744186` for the scene camera, movie layout for a 1720×720 output, and normal shutdown (`Exit (0)`) after about 204 seconds. All 109,394 RunPushBuffer submissions were accepted. These counters do not prove every draw or resource is correct.

No physical ultrawide fullscreen test, exhaustive video/campaign/arena coverage or full graphics regression is claimed. The user's approval applies to the reported windowed test. Original assets, prepared game-code DLLs, saves, raw logs and supplied screenshots are excluded from this source update.

The new portable ZIP passed its allowlist/hash audit and the synthetic native-override/import/exception fixture. A fresh extraction compiled all 129 generated translation units and loaded its local DLL at 50.243 seconds, with `PATH` limited to Windows directories and developer include/library variables removed. The diagnostic exited with code 0 without starting gameplay. This is one preparation measurement on the same PC, not a timing guarantee. ZIP SHA-256: `313b33b1cfc413cf793e8d6746135ef1ea7ba3eba821f21ceade8d04d5224f09`.

## Separate DLL and portable compiler — 19–20 October 2026

The first-launch compiler changes were compiled in a separate test workspace using the existing supported game data. Production saves were not copied into or used by the isolated package tests. The host EXE was about 4.7 MB; game instruction bodies were excluded from it and prepared into a separate local DLL.

Checks completed on the existing Windows/NVIDIA development PC:

- A synthetic game-independent fixture passed native override, fallback/original-body calls, runtime import and cross-DLL exception assertions. A `remainder` math check was added for the operation used by this game's generated x87 code.
- The packaging allowlist and all ZIP manifest hashes passed. No original XBE/data, prepared game-code DLL, generated work files, PDB, saves or mods were included.
- An extracted package in a path containing spaces prepared all 129 generated translation units with `PATH` restricted to Windows system directories and developer include/library environment variables removed. This intermediate candidate took **48.01 seconds**, exited with code 0 and loaded its locally produced DLL. A cached diagnostic reopening took **0.10 seconds** and left the DLL hash unchanged. These are measured preparation examples on this PC, not full launch timings or promises for other systems.
- A forced interruption retained three completed objects. Restart prepared the remaining 126 and loaded the DLL successfully.
- A cached run succeeded with `clang.exe` temporarily absent. Missing compiler plus missing DLL returned an error with a diagnostic. Restoring the compiler repaired the missing DLL using existing objects (`compiling 0 files`).
- Native boot from the locally generated DLL verified the XBE, initialized isolated saves/mods, D3D12 and the original shadow buffer, and followed the logo-skip paths. A bounded 45-second smoke process remained alive until intentionally stopped by the test driver; no natural exit or save-round-trip result is claimed for that check.
- The final ZIP passed the manifest/hash check and a normal first launch with the progress window in a folder containing spaces and an accented character. After correcting TOML/path handling, it compiled all 129 files, loaded the local DLL at 57.86 seconds and reached D3D12/logo-skip initialization at 82.33 seconds. Settings were loaded from that same folder. The test driver then intentionally stopped the process; this was an automatic boot check, not a hands-on gameplay session.
- A deliberately altered copy of the XBE was rejected before preparation; no game-code DLL was produced. The original game files were preserved.
- The final package's cached diagnostic reopening completed with exit code 0 in 0.23 seconds and left the DLL hash unchanged. The complete developer build also linked both the host EXE and the separate game-code DLL successfully.

Final Windows ZIP SHA-256: `466edbcf15413caa04a35db7645c1224a22373b2df27795e97f340902fa6d7a5`.

This environment still has development software installed globally; the restricted process environment and freestanding compiler flags check independence from those tool paths/headers/libraries. A separate PC without development software installed remains untested. Earlier hands-on gameplay/menu/save validation below predates the new DLL layout and does not establish a full regression of this package.

## Fresh-folder build — 19 October 2026

The maintainer ran `build.bat` in the separately prepared public source folder using their own game files. Tested source revision: **`ebbc92e4085c31e453fa1b8100a0253d82083c90`**. This was a fresh source preparation with no old build/generated output copied by the preparation process; it used the same development PC, not an independent machine.

Result: **build successful**. The supplied console output ends with `Linking CXX executable MarvelNemesis.exe`, the success message and the optional launch prompt. The generated executable is present. Ninja records code generation and compilation/linking; no compiler error, CMake error or stopped-build marker was found in the supplied output.

Observed tools:

- Clang 23.1.3.
- CMake 4.3.1-msvc1.
- Windows SDK 10.0.26100.0.
- Visual Studio installation used by the build: 18.11.12306.390 in the recorded environment; Ninja bundled with Visual Studio.

The Ninja log spans about **89 seconds** of logged work. This excludes preceding dependency downloads/configuration and user prompts, and is **not** a measured total first-build time or a performance claim for other PCs.

## Warnings retained

The supplied output includes Clang warnings in fetched SDL/ImGui sources: deprecated functions, an unused global and non-trivial memcpy/memset diagnostics. Zydis and Zycore emit CMake deprecation warnings about future removal of compatibility with older CMake policies. These diagnostics did not stop this build; their presence does not itself establish a gameplay defect or prove they are harmless in every toolchain.

No dependency update, warning suppression or renderer change was made in response. Raw console output and build metadata are kept privately because they include machine paths; generated game code and binaries remain excluded from Git.

## Gameplay smoke check — session 09:28:03

The user supplied a gameplay log from the public build and reported that everything tested worked normally. The executable hash matches the fresh build recorded above. The session starts with default graphics settings and a new portable save directory, without legacy replacement or mod override messages.

The log records the supported XBE verified, PC Settings opened and closed with the original Options page restored, configuration applied, a focus pause/resume cycle including audio, and scene rendering. Later settings include MSAA 8x, SSAO and object distance 3x. Shutdown is normal at 247.751 seconds (`Exit (0)`): 28,201 calls/submissions, zero rejected, empty or skipped buffers.

The native test-scene preflight messages do not prove those scenes were entered. The user's general approval does not enumerate every input, audio or menu path individually. The counters are submission diagnostics, not a performance benchmark or proof of every draw's visual correctness.

## Save round trip — session 09:36:54

After the initial gameplay check, the user explicitly reported manually saving, closing the game, opening it again and successfully loading the save. The supplied reopening log verifies the same supported XBE, uses the existing portable `saves` directory without replacing legacy files, restores configured options and ends normally (`Exit (0)`) at 35.021 seconds. It contains zero RunPushBuffer submissions; this is a short reopening/menu session, not another gameplay-rendering test.

**Result: save/close/reopen/load approved by the user.** The ordinary log does not provide an individual completed-save/load event or byte-for-byte save verification; successful loading is the user's observation. No game or save file was included in the public documentation, and no gameplay was executed by the preparation agent.

## Remaining validation

The build and smoke/save checks were performed on the same development PC. Broader campaign/arena coverage, AMD/Intel hardware, ISO mods and autosave restoration after Development Load/reset remain open as listed in KNOWN_ISSUES.md. The earlier development build has additional reported coverage; do not transfer all those results automatically to a new binary.
