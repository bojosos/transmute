"""Downloads the conformance sources listed in sources.json from their official upstreams.

Each source goes into its own new directory <sources>/<dir>/: the archive, SHA256, SOURCE_URL and
the extracted src/. The archive must match the pinned SHA-256, or nothing is extracted. Directories
that already exist are left alone. The archives are untrusted data: nothing here runs them.

Usage: python -I fetch_sources.py --sources C:\\dev\\conformance [name ...]
"""
import argparse
import hashlib
import json
import pathlib
import shutil
import sys
import tarfile
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent


def extract(archive, dest):
    mode = "r:xz" if archive.name.endswith(".xz") else "r:gz"
    with tarfile.open(archive, mode) as tar:
        members = []
        for m in tar.getmembers():
            parts = pathlib.PurePosixPath(m.name).parts[1:]  # --strip-components=1
            if not parts or m.issym() or m.islnk() or ".." in parts or m.name.startswith("/"):
                continue  # links are not needed by any suite, and are refused on Windows anyway
            m.name = "/".join(parts)
            members.append(m)
        tar.extractall(dest, members=members, filter="data")


def copy_copyleft(manifest, sources, dest):
    """Copies, for every source marked ship_with_binaries in sources.json, the pinned archive (SHA-256
    checked again), SHA256, SOURCE_URL and its licence text into dest/<dir>/, so the source travels
    with programs built from it: GPL-2.0 section 3(a) for Open POSIX, and APSL-2.0 for Apple's test
    suites (conservatively section 2.2, since the build adapts how the tests are compiled).
    licence_file is "<source>:<path in its src/>"; libpthread's release has no licence file of its
    own, so it takes the same APSL-2.0 text from Libc."""
    for name, entry in manifest.items():
        if not entry.get("ship_with_binaries"):
            continue
        src = sources / entry["dir"]
        archive = src / entry["url"].rsplit("/", 1)[1]
        got = hashlib.sha256(archive.read_bytes()).hexdigest()
        if got != entry["sha256"]:
            raise SystemExit(f"{name}: {archive} does not match its pinned SHA-256")
        owner, licence = entry["licence_file"].split(":", 1)
        licence_path = sources / manifest[owner]["dir"] / "src" / licence
        out = dest / entry["dir"]
        out.mkdir(parents=True, exist_ok=True)
        for f in (archive, src / "SHA256", src / "SOURCE_URL", licence_path):
            shutil.copyfile(f, out / f.name)
        print(f"{name} ({entry['licence']}): {out / archive.name}, {licence_path.name}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sources", required=True, type=pathlib.Path)
    ap.add_argument("--copy-copyleft", type=pathlib.Path, metavar="DIR",
                    help="instead of downloading, copy the sources marked ship_with_binaries "
                         "(archive and licence text) into DIR")
    ap.add_argument("names", nargs="*")
    args = ap.parse_args()
    manifest = json.loads((HERE / "sources.json").read_text())
    if args.copy_copyleft:
        copy_copyleft(manifest, args.sources, args.copy_copyleft)
        return
    failed = False
    for name, entry in manifest.items():
        if args.names and name not in args.names:
            continue
        target = args.sources / entry["dir"]
        if target.exists():
            print(f"{name}: {target} exists")
            continue
        url, want = entry["url"], entry["sha256"]
        archive = target / url.rsplit("/", 1)[1]
        target.mkdir(parents=True)
        with urllib.request.urlopen(url, timeout=120) as response, open(archive, "wb") as f:
            shutil.copyfileobj(response, f)
        got = hashlib.sha256(archive.read_bytes()).hexdigest()
        if got != want:
            print(f"{name}: SHA-256 mismatch ({got}, pinned {want}); not extracted", file=sys.stderr)
            failed = True
            continue
        (target / "SHA256").write_text(f"{got} *{archive.name}\n")
        (target / "SOURCE_URL").write_text(url + "\n")
        extract(archive, target / "src")
        print(f"{name}: {target}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
