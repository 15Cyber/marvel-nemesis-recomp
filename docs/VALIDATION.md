# Public alpha candidate validation

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

The build and smoke/save checks were performed on the same development PC. Broader campaign/arena coverage, AMD/Intel hardware, ISO mods and autosave restoration after Development Load/reset remain open as listed in KNOWN_ISSUES.md. The earlier development build has additional reported coverage; do not transfer all those results automatically to this new binary. No publication has taken place.
