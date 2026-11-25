"""Build the self-extracting Windows x64 distribution using the working app venv."""

import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import uuid
import zipfile


ROOT = Path(__file__).resolve().parents[1]
SKIP_DIRS = {"__pycache__", ".git", ".pytest_cache"}
SKIP_SUFFIXES = {".pyc", ".pyo", ".nbc", ".nbi", ".whl"}


def tree(source, destination, excluded=()):
    for folder, dirs, names in os.walk(source):
        current = Path(folder)
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS and current / d not in excluded)
        for name in sorted(names):
            path = current / name
            if path.suffix.lower() not in SKIP_SUFFIXES:
                yield (destination / path.relative_to(source)).as_posix(), path


def package_files(launcher):
    base = Path(sys.base_prefix)
    runtime = Path("runtime")
    files = {"FaunaStudio.exe": launcher}
    for pattern in ("python.exe", "pythonw.exe", "*.dll", "LICENSE.txt"):
        for path in base.glob(pattern):
            files[(runtime / path.name).as_posix()] = path
    # App-local VC runtime for Qt, CPython and native wheels on fresh PCs.
    redist = Path(os.environ["ProgramFiles"]) / "Microsoft Visual Studio"
    crt = max(redist.glob("*/*/VC/Redist/MSVC/*/x64/Microsoft.VC*.CRT"),
              key=lambda d: tuple(map(int, d.parents[1].name.split("."))))
    for path in crt.glob("*.dll"):
        files[(runtime / path.name).as_posix()] = path
    files.update(tree(base / "DLLs", runtime / "DLLs"))
    files.update(tree(base / "Lib", runtime / "Lib", (base / "Lib/site-packages",)))
    packages = Path(sys.prefix) / "Lib/site-packages"
    if not (packages / "torch/lib/torch_cuda.dll").is_file():
        raise RuntimeError("Run this build with the app's CUDA-enabled Windows venv.")
    files.update(tree(packages, runtime / "Lib/site-packages"))
    for name in ("assets", "examples", "viewer", "vendor"):
        files.update(tree(ROOT / name, Path("app") / name))
    for path in [*ROOT.glob("*.py"), ROOT / "README.md"]:
        if path.name != "bootstrap.py":
            files["app/" + path.name] = path
    return files


def compile_launcher(work):
    generated = work / "Release.cs"
    generated.write_text('internal static class Release { internal const string Id = "v1-' +
                         uuid.uuid4().hex[:12] + '"; }', encoding="utf-8")
    launcher = work / "launcher.exe"
    compiler = Path(os.environ["WINDIR"]) / "Microsoft.NET/Framework64/v4.0.30319/csc.exe"
    subprocess.run([str(compiler), "/nologo", "/target:winexe", "/platform:x64", "/optimize+",
                    "/codepage:65001", "/r:System.Windows.Forms.dll", "/r:System.Drawing.dll",
                    "/r:System.IO.Compression.dll", "/r:Microsoft.CSharp.dll", "/out:" + str(launcher),
                    str(ROOT / "packaging/Launcher.cs"), str(generated)], check=True)
    return launcher


def main():
    if sys.platform != "win32" or struct.calcsize("P") != 8 or sys.prefix == sys.base_prefix:
        raise RuntimeError("Use the Windows x64 application virtual environment.")
    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    target = dist / "FaunaStudio-Setup-x64.exe"
    with tempfile.TemporaryDirectory(prefix=".fauna-build-", dir=dist) as temp:
        work = Path(temp)
        launcher = compile_launcher(work)
        files = package_files(launcher)
        output = work / target.name
        output.write_bytes(launcher.read_bytes())
        total = sum(path.stat().st_size for path in files.values())
        print(f"{len(files):,} files, {total / 1e9:.2f} GB before compression", flush=True)
        digest = hashlib.sha256()
        processed = 0
        next_report = 0
        with zipfile.ZipFile(output, "a", zipfile.ZIP_DEFLATED, compresslevel=1) as archive:
            for name, path in sorted(files.items()):
                digest.update((name + "\0").encode("utf-8"))
                with path.open("rb") as source, archive.open(name, "w", force_zip64=True) as entry:
                    while chunk := source.read(1024 * 1024):
                        digest.update(chunk)
                        entry.write(chunk)
                        processed += len(chunk)
                        if processed >= next_report:
                            print(f"Packed {processed / total:.0%}", flush=True)
                            next_report += total // 10
            archive.writestr("payload.sha256", digest.hexdigest())
        if output.stat().st_size >= 4 * 1024**3:
            raise RuntimeError("The package exceeds the Windows single executable size limit.")
        os.replace(output, target)
    print(f"{target} ({target.stat().st_size:,} bytes)", flush=True)


if __name__ == "__main__":
    main()
