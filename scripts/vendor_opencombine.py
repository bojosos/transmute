"""Vendors OpenCombine's code into third_party/opencombine as the Combine module.

Usage: python -I scripts/vendor_opencombine.py <OpenCombine-<tag>.tar.gz> <third_party/opencombine>

From the upstream source archive (github.com/OpenCombine/OpenCombine, pinned in
PROVENANCE.md) it copies only Sources/OpenCombine and Sources/COpenCombineHelpers
plus LICENSE, and applies the recorded mechanical changes:
  1. every documentation comment (`///` lines and `/** ... */` blocks) is
     removed: upstream's carry documentation text that is not OpenCombine's;
  2. the module qualifier `OpenCombine.` becomes `Combine.` (built as Combine);
  3. `@inlinable` is removed: upstream is not written for library evolution,
     and the attribute does not change any exported name.
The root files RemainingCombineInterface.swift and RemainingFoundationInterface.swift,
the tests and the other targets are not copied. Prints the SHA-256 of each
vendored file and of that sorted list (the tree hash), as PROVENANCE.md records
them.
"""
import hashlib
import os
import re
import sys
import tarfile

SOURCES = ("Sources/OpenCombine/", "Sources/COpenCombineHelpers/")
TEXT = (".swift", ".h", ".c", ".cpp")
SKIP = (".gyb",)   # their GENERATED-*.swift outputs are vendored instead


def strip_doc_comments(text: str) -> str:
    out, i, n = [], 0, len(text)
    line, doc_only, in_string = [], False, False
    while i < n:
        c = text[i]
        if in_string:
            line.append(c)
            if c == '\\' and i + 1 < n:
                line.append(text[i + 1]); i += 2; continue
            in_string = c not in '"\n'
            i += 1
            continue
        if c == '"':
            in_string = True; line.append(c); i += 1; continue
        if text.startswith('/**', i) and not text.startswith('/**/', i):
            end = text.find('*/', i + 3)
            i = n if end < 0 else end + 2
            doc_only = True
            continue
        if text.startswith('///', i):
            end = text.find('\n', i)
            i = n if end < 0 else end
            doc_only = True
            continue
        if text.startswith('//', i):
            end = text.find('\n', i)
            end = n if end < 0 else end
            line.append(text[i:end]); i = end
            continue
        if c == '\n':
            content = ''.join(line)
            if content.strip() or not doc_only:
                out.append(content + '\n')
            line, doc_only = [], False
            i += 1
            continue
        line.append(c)
        i += 1
    if line:
        out.append(''.join(line))
    return ''.join(out)


def transform(text: str) -> str:
    text = strip_doc_comments(text)
    text = re.sub(r'\bOpenCombine\.', 'Combine.', text)
    text = re.sub(r'@inlinable[ \t]*\n?[ \t]*', '', text)
    return text


def main() -> None:
    archive, output = sys.argv[1], sys.argv[2]
    manifest = []
    with tarfile.open(archive, 'r:gz') as tar:
        for member in tar.getmembers():
            if not member.isfile():
                continue
            relative = member.name.split('/', 1)[1] if '/' in member.name else member.name
            if relative == 'LICENSE':
                target = 'LICENSE'
            elif relative.startswith(SOURCES) and not relative.endswith(SKIP):
                target = relative
            else:
                continue
            data = tar.extractfile(member).read()
            if target.endswith(TEXT):
                data = transform(data.decode('utf-8')).encode('utf-8')
            path = os.path.join(output, target)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, 'wb') as stream:
                stream.write(data)
            manifest.append(f"{hashlib.sha256(data).hexdigest()}  {target}")
    manifest.sort(key=lambda line: line.split('  ', 1)[1])
    listing = '\n'.join(manifest) + '\n'
    print(listing, end='')
    print(f"tree {hashlib.sha256(listing.encode('ascii')).hexdigest()} ({len(manifest)} files)")


if __name__ == '__main__':
    main()
