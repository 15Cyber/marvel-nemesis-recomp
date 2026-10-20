"""Run from a VS developer shell. Uses only synthetic code, never game fixtures."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile


def run(*args, cwd):
    subprocess.run([str(a) for a in args], cwd=cwd, check=True)


def fixture(root, work):
    clang = shutil.which("clang++")
    lld = shutil.which("lld-link")
    assert clang and lld, "Clang and LLD must be in PATH"
    exports = "LIBRARY Fixture.dll\nEXPORTS\n X86FuncMappings DATA\n __imp__sub_00001000\n __imp__sub_00002000\n TestCallOverride\n TestThrow\n"
    (work / "exports.def").write_text(exports)
    (work / "imports.def").write_text(exports + " sub_00001000 == __imp__sub_00001000\n sub_00002000 == __imp__sub_00002000\n")
    run(lld, "/lib", "/machine:x64", "/def:imports.def", "/out:code.lib", cwd=work)
    include = root / "sdk/runtime"
    run(clang, "-std=c++20", "-DX86_HOST_CODE", "-I", include, root / "tests/dll_host.cpp",
        "code.lib", "-ldelayimp", "-fuse-ld=lld", "-Wl,/delayload:Fixture.dll",
        "-Wl,/implib:Host.lib", "-o", "Host.exe", cwd=work)
    run(clang, "-std=c++20", "-fms-compatibility-version=19.40", "-DX86_GAME_CODE", "-nostdinc", "-nostdinc++",
        "-isystem", include / "cpu/freestanding", "-I", include, "-c", root / "tests/dll_code.cpp", "-o", "code.obj", cwd=work)
    run(lld, "/dll", "/noentry", "/nodefaultlib", "/machine:x64", "/def:exports.def", "/out:Fixture.dll",
        "code.obj", "Host.lib", cwd=work)
    run(work / "Host.exe", cwd=work)
    print("PASS: native overrides, fallback bodies, direct original calls, runtime imports and cross-DLL exceptions")


def package(path):
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        manifest = json.loads(archive.read("manifest.json").decode("utf-8-sig"))
        expected = {entry["path"] for entry in manifest} | {"manifest.json"}
        assert set(names) == expected and len(names) == len(expected)
        forbidden = {".xbe", ".iso", ".dds", ".xpr", ".pdb", ".obj", ".cpp"}
        for name in names:
            assert Path(name).suffix.lower() not in forbidden, name
            assert not {"work", "generated", "saves", "mods", "logs", "captures"}.intersection(Path(name).parts), name
        assert "MarvelNemesis.dll" not in names
        assert "MarvelNemesis.exe" in names and "compiler/MarvelNemesis.lib" in names
        for entry in manifest:
            assert hashlib.sha256(archive.read(entry["path"])).hexdigest() == entry["sha256"], entry["path"]
    print("PASS: package allowlist, hashes, no game-code DLL/data/generated files or saves")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    test_directory = root / "out/tests"
    test_directory.mkdir(parents=True, exist_ok=True)
    fixture(root, Path(tempfile.mkdtemp(prefix="dll-", dir=test_directory)))
    if args.package:
        package(args.package)
