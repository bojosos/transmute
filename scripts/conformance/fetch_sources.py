"""Downloads the conformance sources listed in sources.json from their official upstreams.

Each source goes into its own new directory <sources>/<dir>/: the archive, SHA256 (of the archive as
downloaded), SOURCE_URL and the extracted src/. Directories that already exist are left alone. The
archives are untrusted data: nothing here runs them.

Two kinds of pin:
- "sha256": the archive's bytes. Only for files the upstream uploads once, such as LTP's release
  asset.
- "tree_sha256": the extracted content (tree_digest below). Used for archives the host generates on
  request from a git tag or commit (GitHub archive/refs/tags, the cgit snapshot of libc-test). Their
  content is fixed by the tag or commit, but their bytes are not: compression and timestamps can
  change. The archive is extracted into a staging directory, and becomes src/ only if the digest
  matches.

Usage:
  python -I fetch_sources.py --sources C:\\dev\\conformance [name ...]
  python -I fetch_sources.py --sources C:\\dev\\conformance --digest name ...   (print tree_sha256 pins)
"""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
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


def tree_digest(root):
    """SHA-256 over every regular file under root, sorted by path: "<posix path>\\0<sha256 hex>\\n"
    per file. It depends only on the files' paths and bytes."""
    h = hashlib.sha256()
    for path in sorted(p for p in pathlib.Path(root).rglob("*") if p.is_file()):
        rel = path.relative_to(root).as_posix()
        h.update(f"{rel}\0{hashlib.sha256(path.read_bytes()).hexdigest()}\n".encode())
    return h.hexdigest()


def archive_name(entry):
    return entry["url"].rsplit("/", 1)[1]


def copy_copyleft(manifest, sources, dest):
    """Copies, for every source marked ship_with_binaries in sources.json, the archive that was
    downloaded and verified (checked again against its SHA256 file), SHA256, SOURCE_URL and its licence
    text into dest/<dir>/. The source then travels with programs built from it: GPL-2.0 section 3(a)
    for Open POSIX, and APSL-2.0 for Apple's test suites (conservatively section 2.2, since the build
    adapts how the tests are compiled). licence_file is "<source>:<path in its src/>"; libpthread's
    release has no licence file of its own, so it takes the same APSL-2.0 text from Libc."""
    for name, entry in manifest.items():
        if not entry.get("ship_with_binaries"):
            continue
        src = sources / entry["dir"]
        archive = src / archive_name(entry)
        recorded = (src / "SHA256").read_text().split()[0]
        if hashlib.sha256(archive.read_bytes()).hexdigest() != recorded:
            raise SystemExit(f"{name}: {archive} changed since it was downloaded and verified")
        owner, licence = entry["licence_file"].split(":", 1)
        licence_path = sources / manifest[owner]["dir"] / "src" / licence
        out = dest / entry["dir"]
        out.mkdir(parents=True, exist_ok=True)
        for f in (archive, src / "SHA256", src / "SOURCE_URL", licence_path):
            shutil.copyfile(f, out / f.name)
        print(f"{name} ({entry['licence']}): {out / archive.name}, {licence_path.name}")


def fetch_git(name, entry, target):
    """A source pinned to a git commit (repo.or.cz serves its web snapshots only behind a bot check,
    which a script must not get around; its git service needs none). Clones without checking out,
    checks out the pinned commit, drops .git and verifies the content digest. Git runs no code from
    the clone: hooks are not cloned, and a filter named in .gitattributes does nothing without local
    configuration. Any line-ending conversion would change the digest, so it cannot pass unnoticed."""
    staging = target / "src.staging"
    target.mkdir(parents=True)
    git = ["git", "-c", "core.autocrlf=false", "-c", "core.symlinks=false"]
    for cmd in ([*git, "clone", "-q", "--no-checkout", entry["git"], str(staging)],
                [*git, "-C", str(staging), "checkout", "-q", "--detach", entry["commit"]]):
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            return f"{name}: {' '.join(cmd[3:5])} failed: {proc.stderr.strip()}"
    head = subprocess.run([*git, "-C", str(staging), "rev-parse", "HEAD"], capture_output=True,
                          text=True).stdout.strip()
    if head != entry["commit"]:
        return f"{name}: checked out {head}, pinned {entry['commit']}"
    shutil.rmtree(staging / ".git", onerror=lambda f, p, e: (os.chmod(p, 0o700), f(p)))
    digest = tree_digest(staging)
    if digest != entry["tree_sha256"]:
        shutil.rmtree(staging)
        return f"{name}: content digest mismatch ({digest}, pinned {entry['tree_sha256']}); not kept"
    staging.rename(target / "src")
    (target / "SHA256").write_text(f"{digest} tree of commit {head}\n")
    (target / "SOURCE_URL").write_text(f"{entry['git']} {head}\n")
    return None


def fetch(name, entry, target):
    """Returns an error message, or None."""
    if "git" in entry:
        return fetch_git(name, entry, target)
    url = entry["url"]
    archive = target / archive_name(entry)
    target.mkdir(parents=True)
    with urllib.request.urlopen(url, timeout=120) as response, open(archive, "wb") as f:
        shutil.copyfileobj(response, f)
    got = hashlib.sha256(archive.read_bytes()).hexdigest()
    if "sha256" in entry and got != entry["sha256"]:
        return f"{name}: archive SHA-256 mismatch ({got}, pinned {entry['sha256']}); not extracted"
    staging = target / "src.staging"
    extract(archive, staging)
    if "tree_sha256" in entry:
        digest = tree_digest(staging)
        if digest != entry["tree_sha256"]:
            shutil.rmtree(staging)
            return f"{name}: content digest mismatch ({digest}, pinned {entry['tree_sha256']}); not extracted"
    staging.rename(target / "src")
    (target / "SHA256").write_text(f"{got} *{archive.name}\n")
    (target / "SOURCE_URL").write_text(url + "\n")
    return None


def print_digests(manifest, sources, names):
    """tree_sha256 pins for already-downloaded sources, computed from their archives exactly as
    fetch() checks them."""
    for name in names:
        entry = manifest[name]
        if "git" in entry:
            print(f"{name}\t{tree_digest(sources / entry['dir'] / 'src')}")
            continue
        archive = sources / entry["dir"] / archive_name(entry)
        with tempfile.TemporaryDirectory() as tmp:
            extract(archive, pathlib.Path(tmp) / "src")
            print(f"{name}\t{tree_digest(pathlib.Path(tmp) / 'src')}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sources", required=True, type=pathlib.Path)
    ap.add_argument("--copy-copyleft", type=pathlib.Path, metavar="DIR",
                    help="instead of downloading, copy the sources marked ship_with_binaries "
                         "(archive and licence text) into DIR")
    ap.add_argument("--digest", action="store_true", help="print tree_sha256 pins of downloaded sources")
    ap.add_argument("names", nargs="*")
    args = ap.parse_args()
    manifest = json.loads((HERE / "sources.json").read_text())
    if args.copy_copyleft:
        copy_copyleft(manifest, args.sources, args.copy_copyleft)
        return
    if args.digest:
        print_digests(manifest, args.sources, args.names or list(manifest))
        return
    failed = False
    for name, entry in manifest.items():
        if args.names and name not in args.names:
            continue
        target = args.sources / entry["dir"]
        if target.exists():
            print(f"{name}: {target} exists")
            continue
        error = fetch(name, entry, target)
        if error:
            print(error, file=sys.stderr)
            failed = True
        else:
            print(f"{name}: {target}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
