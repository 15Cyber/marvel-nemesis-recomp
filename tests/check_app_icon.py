"""Verify icon frames, reproducible SVG rendering and the existing window resource ID without compiling."""
from pathlib import Path
import importlib.util
import io
import re
import sys
from PIL import Image

sys.dont_write_bytecode = True
root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('make_app_icon', root / 'tools/make_app_icon.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
expected = io.BytesIO()
module.save_icon(module.render(root / 'resources/app-icon.svg'), expected)
assert expected.getvalue() == (root / 'resources/app-icon.ico').read_bytes(), 'Regenerate the ICO from its SVG'
with Image.open(root / 'resources/app-icon.ico') as icon:
    assert icon.ico.sizes() == {(s, s) for s in module.SIZES}
    for size in icon.ico.sizes():
        frame = icon.ico.getimage(size)
        assert frame.mode == 'RGBA'
        # Lanczos can leave one alpha level at a corner in the 20 px frame.
        # The exporter clears all four corners after resizing each frame.
        assert all(frame.getpixel(corner)[3] == 0 for corner in (
            (0, 0), (size[0] - 1, 0), (0, size[1] - 1), (size[0] - 1, size[1] - 1)))
        assert frame.getextrema()[3] == (0, 255)
resource = (root / 'resources/app.rc').read_text()
assert re.search(r'^1\s+ICON\s+"app-icon\.ico"', resource, re.M)
for name in ('main.cpp', 'splash.cpp'):
    assert 'LoadIconW(inst, MAKEINTRESOURCEW(1))' in (root / 'sdk/runtime/app' / name).read_text()
cmake = (root / 'CMakeLists.txt').read_text()
assert 'enable_language(RC)' in cmake
assert 'target_sources(MarvelNemesis PRIVATE resources/app.rc)' in cmake
assert 'OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/resources/app-icon.ico' in cmake
print('PASS: nine RGBA icon sizes, transparent corners, deterministic SVG source and window/splash resource ID')
