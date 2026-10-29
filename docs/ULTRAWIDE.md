# Ultrawide 21:9

Support for [issue #1](https://github.com/15Cyber/marvel-nemesis-recomp/issues/1). **The local Release build and geometry checks passed, and I checked menus and combat in a 1720×720 window on 21 October 2026.** The earlier alpha.2 Windows package predates this change.

The test used a 16:9 desktop; 1720×720 has exactly the same aspect ratio as 3440×1440. Supplied screenshots show the scene across the full window and HUD elements inside the centred 16:9 area. The native black bars during the fighter introduction remain inside that same area, leaving the sides visible; I accepted this behaviour and found no interference with combat. Fullscreen on a physical ultrawide monitor and exhaustive movie/campaign/effect coverage remain untested. See [Validation](VALIDATION.md).

## Intended behaviour

- Automatic framing for wide output resolutions such as 3440×1440 and 2560×1080, using the actual pixel ratio.
- The main 3D camera gains horizontal visibility while retaining its vertical field of view and depth projection. Native CPU frustum bounds and inverse matrices follow the wider projection.
- HUD and menus retain a centred 16:9 canvas. At 3440×1440, that canvas is 2560 pixels wide with 440-pixel margins. The F10 panel remains in native window pixels.
- Character previews, auxiliary cameras and smaller render targets retain their existing framing. Unknown/custom scene projections fall back to the original centred frame.
- On ultrawide outputs, Original video framing automatically fits the decoded VP6 frame without stretching or cropping. Fit Aspect and Fit 4:3 preserve the whole image; Fill/Crop preserves proportions but may crop edges. The movie background covers the complete output.
- Standard 16:9 and 4:3 modes retain their existing presentation. A deliberately selected 4:3 console mode retains its original canvas even on a wide desktop.

## Building and checking

Follow [Building](BUILDING.md) and run `build.bat` from this source tree with your supported game files. Reopening an older published EXE does not add the new host renderer and camera hooks.

On a 21:9 monitor, select 3440×1440 or 2560×1080. For an initial check on a 16:9 desktop, a 1720×720 window has the same aspect ratio as 3440×1440:

```toml
display_mode = "windowed"
width = 1720
height = 720
```

Back up your settings before editing them. Apply these keys in the existing `settings.toml` beside the new EXE.

Use the following checklist for broader coverage beyond the approved menu/combat test:

1. Compare the same scene at 16:9 and 21:9. The wider view should reveal more to the sides while retaining vertical framing and character proportions.
2. Move actors/objects across the newly visible edges. Check frustum clipping, particles, projected effects and character shadows.
3. Check character selection, Options/PC Settings, pause, both players' HUD and clipped/scrolling text. Menus and HUD should remain centred and undistorted.
4. Play a logo and a campaign video in Original/Fit modes. Check their proportions, black margins and transitions. Try Fit 4:3/Fill if used, and switch 21:9 → 16:9 → 21:9.
5. Check a Versus arena and a campaign scene, including the graphics options you use. Wider in-engine cinematics can reveal scenery outside the original composition.

The log reports `ultrawide: scene ... horizontal_scale=...` for the supported camera path, or a centred-frame fallback for unknown projections. F3 draw captures include `frame`: X=1 for the main world/movie canvas and the compensation factor for the interface.

## Isolated geometry checks

`tests/ultrawide.cpp` checks the production layout/projection/video helpers without game data, a window or a GPU. Cases cover 3440×1440, 2560×1080, legacy aspects, UI proportions, horizontal projection/depth invariants, frustum/cone expansion, three video framing modes and invalid projection inputs.

These checks compiled and passed locally on 21 October 2026 with Clang 23.1.3. The Release host and game-code DLL also built successfully, and `--prepare-only` loaded the DLL from a separate test installation with developer environment variables removed. This did not start gameplay or validate the rendered image. To repeat the geometry checks from a Visual Studio developer shell with Clang available, after `out/` exists:

```bat
clang++ -std=c++20 -UNDEBUG -Isdk/runtime -Isrc tests/ultrawide.cpp -o out/ultrawide-check.exe
out\ultrawide-check.exe
```

An assertion failure means the geometry checks failed. Passing them does not establish visual correctness, shader compilation or gameplay compatibility. No 32:9 or multi-monitor validation is claimed.
