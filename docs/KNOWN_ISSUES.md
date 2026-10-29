# Alpha validation and known limitations

## Confirmed coverage

- I compiled the public source candidate in a fresh folder on 19 October 2026, then checked gameplay and a manual save/close/reopen/load cycle. Logs confirm normal shutdown in both supplied runtime sessions. See [Validation](VALIDATION.md).

- Development testing on one Windows 11 system with an NVIDIA RTX 5060 Ti, Ryzen 5 5600 and 32 GB RAM.
- Original and PC menus, multiple Versus arenas/characters, local two-player input, keyboard remapping/coexistence, graphics options and focus pause exercised by the tester.
- Thing, Wolverine and Elektra campaign missions visited through Development, without full objective/completion coverage.
- Development test-scene entry/exit, autosave suppression and manual saving approved in the reported paths.
- Seven legacy save files migrated with matching hashes; migrated save loaded successfully.
- Actual DDS/XPR file override visually approved in Avengers Mansion, with normal process exit and zero recorded RunPushBuffer rejections in that session.

## Pending checks

- Ultrawide 21:9 passed a local Release build, geometry checks and DLL loading; I checked menus and combat in a 1720×720 window on 21 October 2026. Physical ultrawide fullscreen, exhaustive edge visibility, movies/campaign scenes and graphics combinations still need broader testing. Native fighter-introduction black bars remain within the centred 16:9 area; I accepted this in the windowed test. The earlier alpha.2 package predates this feature. See [Ultrawide](ULTRAWIDE.md).

- The portable first-launch flow was checked with developer paths/include/library variables removed on my PC. A friend tested the 29 October portable candidate and approved it for release; I have not received a detailed hardware/build log from that test.
- The new EXE/DLL layout has automated preparation, recovery, synthetic ABI and native boot smoke coverage. The earlier hands-on gameplay/save checks below predate this layout; a full gameplay/save regression of the new package is still welcome.

- Broader second-PC coverage and a documented source build outside my development PC.
- AMD/Intel GPUs, Windows 10, broader campaign and device hot-plug coverage.
- Autosave restoration after normal campaign Load/reset following a Development session. The public fresh-folder save test did not establish this separate behavior.
- Mods applied over an ISO and additional resource/campaign bundle paths.
- Every pause-menu/cancellation path, individual movie scale modes and all cheat failure conditions.
- Object distance visual comparisons and effect costs without a frame-rate limiter. Capped screenshots do not establish performance headroom.

## Technical limits

- Rendering is Direct3D 12 only. Vulkan, native Linux support and HDR are not implemented.
- RunPushBuffer fixups, unsupported command cases and indices beyond 16 bits can still be rejected. Zero rejections in a tested session do not implement these cases.
- USEW is a compatibility adaptation of observed cases, not complete NV2A W-buffer behavior.
- DirectSound Buffer_Pause and SetEG hooks do not implement those original operations. Focus pause is implemented separately.
- MSAA depth sampling uses sample zero in the current path.
- Test scenes are experimental; poses, materials and scripted behavior may be incomplete.
- Development autosave suppression does not sandbox saves or roll back RAM. Manual saves can persist test progress.
- Guest memory remains constrained; arbitrary HD resources or new character/arena dependencies are not automatically safe.
- Bloom is intentionally retained at its current subtle appearance; no new global illumination system was added.

Please report specific reproducible defects with a sanitized log. Do not attach original game files or dumps to public reports.
