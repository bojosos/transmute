"""Copies chosen files verbatim out of an upstream release archive into third_party/<name>/ and
prints the SHA-256 list that PROVENANCE.md records (one "<sha256>  <path>" line per file, sorted,
and the SHA-256 of that list as the tree hash).

Usage: python -I scripts/vendor_files.py <archive> <destination> --strip <prefix/> <member>...
  <member> is a path inside the archive after <prefix/>; a trailing "/" copies a directory, and
  "path/*.c" copies matching files of one directory. The archive is read, never extracted to disk
  as a whole; nothing in it is executed. Existing files in <destination> are replaced, others kept.
"""
import argparse
import fnmatch
import hashlib
import os
import posixpath
import tarfile
import zipfile


def members(archive):
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as z:
            for info in z.infolist():
                if not info.is_dir():
                    yield info.filename, (lambda info=info: z.read(info))
    else:
        with tarfile.open(archive) as t:
            for info in t:
                if info.isfile():
                    yield info.name, (lambda info=info: t.extractfile(info).read())


def wanted(path, patterns):
    for pattern in patterns:
        if pattern.endswith('/') and path.startswith(pattern):
            return True
        if posixpath.dirname(path) == posixpath.dirname(pattern) and fnmatch.fnmatchcase(path, pattern):
            return True
        if path == pattern:
            return True
    return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('archive')
    parser.add_argument('destination')
    parser.add_argument('--strip', default='', help='archive prefix to remove, e.g. libffi-3.8.0/')
    parser.add_argument('member', nargs='+')
    args = parser.parse_args()
    copied = {}
    for name, read in members(args.archive):
        if not name.startswith(args.strip):
            continue
        relative = name[len(args.strip):]
        if not relative or '..' in relative.split('/') or relative.startswith('/') or not wanted(relative, args.member):
            continue
        data = read()
        target = os.path.join(args.destination, *relative.split('/'))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, 'wb') as f:
            f.write(data)
        copied[relative] = hashlib.sha256(data).hexdigest()
    missing = [m for m in args.member if not any(wanted(path, [m]) for path in copied)]
    if missing:
        raise SystemExit('not in the archive: ' + ', '.join(missing))
    listing = ''.join(f'{digest}  {path}\n' for path, digest in sorted(copied.items()))
    print(listing, end='')
    print(f'tree {hashlib.sha256(listing.encode()).hexdigest()} ({len(copied)} files)')


if __name__ == '__main__':
    main()
