# Usage

## Settings and input

Use **Options → PC Settings** for the native English menu or **F10** for the PC panel. Both share settings. Apply/save writes `settings.toml` beside the executable. Display changes have a confirmation timeout; presets and effect controls are independent.

The keyboard and a controller can both drive player 1; a second controller can drive player 2 in Versus. Check Controls for player/device assignment and key/button remapping. Not every hot-plug/device combination has been validated.

## Saves

Game data is stored beside the executable under `saves/E/UDATA/4541038a/`. First startup can copy an old `hdd/E` into `saves/E`; the legacy directory is retained, and populated saves are not merged or overwritten. Close the game before backing up or moving saves. PC settings remain in the separate `settings.toml`.

Migration and loading an existing migrated save were validated. A separate new-save/reopen test after migration is still pending individual confirmation.

## File mods

Put compatible Xbox resources in `mods/` beside the executable, mirroring their paths within the game data. Restart the game after installing/removing files. Matching paths replace original reads; missing paths fall back to original data. XBE overrides are excluded.

Examples of paths only (no files are supplied):

```text
mods/Materials/Models/Spiderman.dds
mods/Bundles/spiderman.xpr
```

Bundled XPR textures may take precedence over loose DDS files; replacing a loose DDS alone may have no effect. A community GameCube PNG was manually adapted to native Xbox DDS/XPR and validated in one arena. Dolphin hashes and PNGs are not imported automatically. Original normal maps/reflection masks can remain visible over a changed color texture.

This layer does not add roster slots, import FBX/glTF or provide a host-side HD texture injector. Guest memory limits and resource dependencies still apply. To disable a mod, close the game and move its files outside `mods`; renaming the whole directory disables all files in it, and an empty directory is recreated on startup.

## Development and cheats

Development is an experimental front-end mission/test selector. It is unavailable from the pause menu. Test actors may have T poses or incomplete behavior. Only three test scenes are exposed; unavailable scenes were removed from the selector.

Loading a Development destination blocks automatic saving for that test session. **It does not revert progress in memory or prevent manual saves.** Load your real campaign save or restart before resuming normal progress; keep a backup. The implemented Load/reset autosave-restoration path still needs individual validation.

Infinite health/energy cheats are session modifiers and do not cover every scripted death, ring-out or objective failure. Unlock actions affect state that the normal save flow can persist. They do not themselves force a new autosave.

## Captures

Start in the executable directory, in PowerShell:

```powershell
$env:XBE_CAPTURE_DIR = 'captures'
.\MarvelNemesis.exe
```

Press F3 at the defect. Capture output can contain game data and guest memory; keep it private until a maintainer specifies what sanitized information is needed. Register/stack messages labeled `error:` after F3 alone do not establish a crash. For public reports, start with a sanitized text log and reproduction steps; see [Contributing](../CONTRIBUTING.md).
