#!/usr/bin/env python3
"""Package a release build into one core zip plus one zip per bundled plugin.

    python tools/package_release.py --dist build/dist --version 1.0.0 --platform windows-x86 --out release

Every zip unpacks into a server's root folder. The core zip holds the loader, rmm/bin, gamedata, the Python
bridge and the core configs. A plugin zip holds rmm/plugins/<name>.dll|.so plus its example configs:
configs/plugins/<name>.cfg and the configs/<name>/ folder, if present.

Test-only files the build drops into dist/rmm/configs/ are never packaged; configs come from the source tree.
"""

import argparse
import shutil
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONFIGS = ROOT / "configs"
PLUGIN_SUFFIXES = (".dll", ".so")
CORE_DOCS = ("README.md",)


def add_file(zf: zipfile.ZipFile, src: Path, arc: str) -> None:
    zf.write(src, arc)
    print(f"  {arc}")


def add_tree(zf: zipfile.ZipFile, src: Path, arc: str) -> None:
    for path in sorted(src.rglob("*")):
        if path.is_file() and "__pycache__" not in path.parts and path.suffix != ".pyc":
            add_file(zf, path, f"{arc}/{path.relative_to(src).as_posix()}")


def plugin_config_paths(name: str) -> list[Path]:
    return [p for p in (CONFIGS / "plugins" / f"{name}.cfg", CONFIGS / name) if p.exists()]


def package_core(dist: Path, out: Path, plugins: list[str]) -> Path:
    rmm = dist / "rmm"
    owned = {p.resolve() for name in plugins for p in plugin_config_paths(name)}
    print(f"{out.name}:")

    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
        # Loader next to the server executable (Windows only).
        for name in ("winmm.dll", "rmm_launcher.exe"):
            if (dist / name).is_file():
                add_file(zf, dist / name, name)

        add_tree(zf, rmm / "bin", "rmm/bin")
        add_tree(zf, rmm / "gamedata", "rmm/gamedata")
        add_tree(zf, rmm / "python", "rmm/python")

        # Core configs: everything in configs/ that no plugin owns.
        for path in sorted(CONFIGS.rglob("*")):
            if not path.is_file():
                continue

            if any(path.resolve() == o or o in path.resolve().parents for o in owned):
                continue

            add_file(zf, path, f"rmm/configs.example/{path.relative_to(CONFIGS).as_posix()}")

        for doc in CORE_DOCS:
            add_file(zf, ROOT / doc, f"rmm/{doc}")

    return out


def package_plugin(dist: Path, binary: Path, out: Path) -> Path:
    name = binary.stem
    print(f"{out.name}:")

    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
        add_file(zf, binary, f"rmm/plugins/{binary.name}")

        for path in plugin_config_paths(name):
            arc = f"rmm/configs.example/{path.relative_to(CONFIGS).as_posix()}"
            if path.is_dir():
                add_tree(zf, path, arc)
            else:
                add_file(zf, path, arc)

    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dist", type=Path, required=True, help="build/dist directory")
    parser.add_argument("--version", required=True, help="release version, e.g. 1.0.0")
    parser.add_argument("--platform", required=True, help="platform tag, e.g. windows-x86 or linux-x86")
    parser.add_argument("--out", type=Path, required=True, help="output directory")
    args = parser.parse_args()

    dist = args.dist.resolve()
    plugin_dir = dist / "rmm" / "plugins"
    binaries = sorted(p for p in plugin_dir.iterdir() if p.suffix in PLUGIN_SUFFIXES) if plugin_dir.is_dir() else []
    if not (dist / "rmm" / "bin").is_dir():
        print(f"error: {dist}/rmm/bin not found - build first", file=sys.stderr)
        return 1

    if not binaries:
        print(f"error: no plugins in {plugin_dir}", file=sys.stderr)
        return 1

    if args.out.exists():
        shutil.rmtree(args.out)

    args.out.mkdir(parents=True)
    suffix = f"{args.version}-{args.platform}.zip"
    package_core(dist, args.out / f"rmm-{suffix}", [b.stem for b in binaries])
    for binary in binaries:
        package_plugin(dist, binary, args.out / f"rmm-plugin-{binary.stem}-{suffix}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
