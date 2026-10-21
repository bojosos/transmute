# SPDX-License-Identifier: MIT
"""Exhaustive import inventory of a macOS bundle against Alchemy's providers.

Post-processes, without running guest code:

* `alchemy-fx doctor --deps --scan-bundle --json BUNDLE` (every Mach-O image,
  every dependency and every imported symbol with the loader's binding
  decision),
* optionally `alchemy_objc_gap_scan.py` output: scan.json (Objective-C
  metadata per image) and gaps.json (selector and class gaps against an
  ALCHEMY_DUMP_OBJC runtime class dump),
* optionally `[alchemy probe]` lines of ALCHEMY_PROBE_MISSING runs (the gaps a
  run actually reached),
* optionally Alchemy's provider sources, for a heuristic "binds, but fails
  explicitly" mark: symbols installed through `unsupported(lib, "_x", ...)`
  (a call stops the process) or `failure(lib, "_x", code)` (a call returns an
  error code), directly or from a `for (name : {...})` list,
* optionally the bundle itself, for C strings that name libraries or
  frameworks (dlopen candidates) and Objective-C class names that no image
  imports (NSClassFromString candidates). Both are heuristics over
  `__TEXT,__cstring`; no code is read.

Every import gets one class:

  provided          bound to an Alchemy provider export
  provided-error    bound; the source heuristic says a call returns an error
  provided-stop     bound; the source heuristic says a call stops the process
  unsupported       bound, but doctor marks the API as deferred (fails at load)
  guest             bound to another image of the bundle or a runtime library
  missing-library   strong import from a library identity Alchemy lacks
  unresolved        strong import the library identity does not export
  weak-unresolved   weak import that binds to NULL
  other             doctor statuses not listed above (kept verbatim)

Usage:
  python -I alchemy_import_inventory.py --doctor doctor.json --out OUT_DIR
      [--scan scan.json] [--gaps gaps.json] [--probe LOG ...] [--sources SRC]
      [--bundle BUNDLE.app] [--summary-md FILE] [--symbols-md FILE]

Writes OUT_DIR/inventory.json, OUT_DIR/imports.csv and OUT_DIR/libraries.csv.
"""
import argparse
import collections
import csv
import json
import os
import re
import struct
import sys

STOP, ERROR = 'provided-stop', 'provided-error'
# Images outside the browser process tree (updater, Keystone, privileged helper).
UPDATER = re.compile(r'GoogleUpdater\.app|GoogleSoftwareUpdate|UpdaterPrivilegedHelper')
ORDER = ['unresolved', 'missing-library', 'unsupported', 'provided-stop', 'provided-error', 'weak-unresolved',
         'provided', 'guest', 'other']


# -- provider sources -----------------------------------------------------------
DIRECT = re.compile(r'\b(unsupported|failure)\(\s*[A-Za-z_:.>-]+\s*,\s*"(_[A-Za-z0-9_$]+)"')
LOOP = re.compile(r'for\s*\(\s*const\s+(?:auto|char)\s*\*?\s*(?:const\s*)?\*?\s*&?\s*(\w+)\s*:\s*'
                  r'(?:std::array<[^>]*>\s*)?\{([^{}]*)\}\s*\)\s*\{?\s*(unsupported|failure)\(\s*[A-Za-z_:.>-]+\s*,\s*(\w+)\b',
                  re.S)
NAME = re.compile(r'"(_[A-Za-z0-9_$]+)"')


def explicit_failures(src):
    """symbol -> 'provided-stop' | 'provided-error' from the provider sources (heuristic)."""
    found = {}
    if not src:
        return found
    for dp, dn, fn in os.walk(src):
        dn[:] = [d for d in dn if d != 'tests']
        for n in fn:
            if not n.endswith(('.cpp', '.inc', '.hpp')):
                continue
            with open(os.path.join(dp, n), encoding='utf-8', errors='replace') as f:
                text = f.read()
            for kind, sym in DIRECT.findall(text):
                found.setdefault(sym, STOP if kind == 'unsupported' else ERROR)
            for var, names, kind, used in LOOP.findall(text):
                if var != used:
                    continue
                for sym in NAME.findall(names):
                    found.setdefault(sym, STOP if kind == 'unsupported' else ERROR)
            # Handlers whose short body only refuses: ENOTSUP / KERN_NOT_SUPPORTED, or a throw
            # that names the call unsupported.
            for match in HANDLER.finditer(text):
                body = text[match.end():match.end() + 400]
                cut = NEXT_HANDLER.search(body)
                body = body[:cut.start()] if cut else body
                # Only short bodies: a long handler that throws on one edge case is an implementation.
                if THROWS_UNSUPPORTED.search(body) and len(body) < 200:
                    found.setdefault(match.group(1), STOP)
                elif REFUSES.search(body) and len(body) < 260:
                    found.setdefault(match.group(1), ERROR)
    return found


HANDLER = re.compile(r'\b(?:raw|fn|install|install_code|function)\(\s*(?:[A-Za-z_:.>-]+\s*,\s*)?"(_[A-Za-z0-9_$]+)"\s*,')
NEXT_HANDLER = re.compile(r'\n\s*(?:raw|fn|install|install_code|function|failure|unsupported|constant)\(')
THROWS_UNSUPPORTED = re.compile(r'throw[^;]*(unsupported|not supported)', re.I)
REFUSES = re.compile(r'\b(e_notsup|kern_not_supported|enotsup|ENOTSUP|e_nosys|enosys)\b')


# -- bundle strings ---------------------------------------------------------------
CPU_ARM64 = 0x0100000C


def arm64_slice(blob):
    magic = blob[:4]
    if magic == b'\xcf\xfa\xed\xfe':
        return (0, len(blob)) if struct.unpack_from('<I', blob, 4)[0] == CPU_ARM64 else None
    if magic in (b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf'):
        wide = magic == b'\xca\xfe\xba\xbf'
        count = struct.unpack_from('>I', blob, 4)[0]
        if count > 64:
            return None
        at = 8
        for _ in range(count):
            if wide:
                cpu, _s, off, size, _a, _r = struct.unpack_from('>IIQQII', blob, at)
                at += 32
            else:
                cpu, _s, off, size, _a = struct.unpack_from('>IIIII', blob, at)
                at += 20
            if cpu == CPU_ARM64:
                return off, size
    return None


def cstring_section(data):
    """Bytes of __TEXT,__cstring (and __TEXT,__objc_classname) of a thin arm64 image."""
    ncmds = struct.unpack_from('<I', data, 16)[0]
    at, out = 32, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, at)
        if cmd == 0x19:
            nsects = struct.unpack_from('<I', data, at + 64)[0]
            s = at + 72
            for _ in range(nsects):
                sect = data[s:s + 16].rstrip(b'\0').decode('ascii', 'replace')
                seg = data[s + 16:s + 32].rstrip(b'\0').decode('ascii', 'replace')
                _addr, ssize, offset = struct.unpack_from('<QQI', data, s + 32)
                if seg == '__TEXT' and sect in ('__cstring', '__objc_classname') and offset:
                    out.append((sect, data[offset:offset + ssize]))
                s += 80
        at += size
    return out


LIBRARY_STRING = re.compile(r'^(/System/Library/(?:Private)?Frameworks/\S+|/usr/lib/\S+\.dylib|/usr/lib/\S+|'
                            r'@(?:rpath|loader_path|executable_path)/\S+|[A-Za-z0-9_+.-]+\.(?:dylib|framework|bundle))$')
CLASS_STRING = re.compile(r'^_?(NS|CA|AV|MTL|MTK|CI|WK|UN|SC|LA|AS|CK|CL|NE|NW|PK|QL|SF|UT|VN|MK|GC|CB|CW|CN|IOBluetooth|'
                          r'BC|LS|SK|SR|TK|EK|PH|MP|CX|CT|OS_)[A-Z][a-z][A-Za-z0-9]{2,}$')
NOT_CLASS = re.compile(r'(Notification|Key|Keys|Attribute|Attributes|Options?|Error|Domain|Identifier|Name|Type|'
                       r'Changed|Did[A-Z]\w*|Will[A-Z]\w*|Mode|Style|Value|Event|Path|Format|Kind|Info|Pboard\w*|'
                       r'Delegate|DataSource|Providing|Protocol|Handling|Context|Callback|Target)$')


def bundle_strings(bundle):
    """Per image: library-like strings and class-like strings of its cstring sections."""
    out, seen = {}, set()
    for dp, dn, fn in os.walk(bundle):
        # Installed framework links are junctions (directories) and hard links (files): visit each once.
        dn[:] = [d for d in dn if not (os.path.islink(os.path.join(dp, d)) or
                                       (hasattr(os.path, 'isjunction') and os.path.isjunction(os.path.join(dp, d))))]
        for n in fn:
            p = os.path.join(dp, n)
            try:
                st = os.stat(p)
                if st.st_size < 32 or os.path.islink(p) or (st.st_dev, st.st_ino) in seen:
                    continue
                seen.add((st.st_dev, st.st_ino))
                with open(p, 'rb') as f:
                    if f.read(4) not in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf'):
                        continue
                with open(p, 'rb') as f:
                    blob = f.read()
            except OSError:
                continue
            sl = arm64_slice(blob)
            if not sl:
                continue
            data = blob[sl[0]:sl[0] + sl[1]]
            libs, classes = set(), set()
            try:
                sections = cstring_section(data)
            except struct.error:
                continue
            for sect, raw in sections:
                for piece in raw.split(b'\0'):
                    if not piece or len(piece) > 300:
                        continue
                    try:
                        text = piece.decode('ascii')
                    except UnicodeDecodeError:
                        continue
                    if sect == '__cstring' and LIBRARY_STRING.match(text):
                        libs.add(text)
                    elif sect == '__cstring' and CLASS_STRING.match(text) and not NOT_CLASS.search(text):
                        classes.add(text)
            out[os.path.relpath(p, bundle).replace('\\', '/')] = {'libraries': sorted(libs), 'class_names': sorted(classes)}
    return out


# -- probe logs -------------------------------------------------------------------
def probe_entries(paths):
    """First-seen `[alchemy probe] ...` lines, deduplicated, per log."""
    out = []
    for path in paths:
        seen, lines = set(), []
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                if '[alchemy probe]' not in line:
                    continue
                text = line.strip()[line.strip().find('[alchemy probe]'):]
                if text not in seen:
                    seen.add(text)
                    lines.append(text)
        out.append({'log': os.path.basename(path), 'entries': lines})
    return out


# -- areas ------------------------------------------------------------------------
# (area, library regex, symbol regex): first match wins. Libraries match the last
# path component of the identity.
AREA_RULES = [
    ('memory-jit', r'.', r'^_(pthread_jit|sys_icache|sys_dcache|mach_vm_|vm_(allocate|deallocate|protect|map|remap|region|copy|read|write|purgable)|mmap|mprotect|munmap|madvise|mincore|malloc_zone|malloc_default|malloc_get_all_zones)'),
    ('crashpad', r'.', r'^_(task_set_exception_ports|task_get_exception_ports|task_swap_exception_ports|thread_(get|set|swap)_exception_ports|host_(get|set|swap)_exception_ports|mach_exc|exc_server|catch_|thread_get_state|thread_set_state|task_threads|task_info|thread_info|task_suspend|task_resume|thread_suspend|thread_resume|mach_vm_read|proc_pidinfo|backtrace)'),
    ('sandbox', r'^(libsandbox|libSystem)', r'^_(sandbox_|___sandbox|___mac_syscall|csops|csr_|_?mac_|rootless)'),
    ('process', r'.', r'^_(mach_|bootstrap_|xpc_|launch_|mig_|task_|thread_policy|thread_switch|semaphore_|port_|fileport_|posix_spawn|responsibility_|audit_token|NDR_|notify_|dispatch_mach|dispatch_source_create|kevent|kqueue|proc_|pid_|host_|clock_|_?kernelrpc)'),
    ('process', r'^(Foundation|CoreFoundation|ServiceManagement|libbsm)', r'(NSXPC|SMJob|SMApp|CFMessagePort|CFMachPort|audit_)'),
    ('keychain-security', r'^(Security|CryptoTokenKit|LocalAuthentication|LocalAuthenticationEmbeddedUI|AuthenticationServices|SecurityInterface|libbsm)', r'.'),
    ('gpu-compositing', r'^(Metal|MetalKit|IOSurface|QuartzCore|OpenGL|SkyLight|libGL|CoreImage)', r'.'),
    ('gpu-compositing', r'^CoreVideo', r'(Metal|IOSurface|DisplayLink|OpenGL)'),
    ('fonts-text', r'^(CoreText)', r'.'),
    ('fonts-text', r'^(CoreGraphics|ApplicationServices)', r'(Font|Glyph|ATS)'),
    ('networking', r'^(SystemConfiguration|Network|CFNetwork|libresolv|GSS|CoreWLAN|OpenDirectory|libnetwork)', r'.'),
    ('networking', r'^libSystem', r'^_(getaddrinfo|getnameinfo|if_|getifaddrs|freeifaddrs|res_|dns_|nw_|socket|connect|bind|listen|accept|send|recv|getsockopt|setsockopt|inet_|htons|ntohs)'),
    ('media', r'^(AVFoundation|AVFAudio|AVKit|CoreMedia|CoreMediaIO|VideoToolbox|AudioToolbox|AudioUnit|CoreAudio|CoreMIDI|ScreenCaptureKit|MediaPlayer|CoreHaptics|GameController|ForceFeedback|MediaAccessibility|CoreVideo|Vision|CoreML|Accelerate)', r'.'),
    ('appkit-ui', r'^(AppKit|Carbon|HIToolbox|ApplicationServices|Accessibility|UserNotifications|SafariServices|CoreGraphics|UniformTypeIdentifiers|CoreServices|LaunchServices|ImageIO)', r'.'),
]


def area_of(library, symbol, images):
    lib = library.split('/')[-1]
    if images and all(i.endswith('chrome_crashpad_handler') for i in images):
        return 'crashpad'
    for area, lrx, srx in AREA_RULES:
        if re.search(lrx, lib) and re.search(srx, symbol):
            return area
    return 'other'


# -- classification ---------------------------------------------------------------
def classify(imp, dep_status, explicit):
    st = imp.get('status')
    if st == 'provider':
        return explicit.get(imp['symbol'], 'provided')
    if st == 'guest':
        return 'guest'
    if st == 'unsupported':
        return 'unsupported'
    if st == 'weak-null' or (st == 'unresolved' and imp.get('weak')):
        return 'weak-unresolved'
    if st in ('unresolved', 'invalid-ordinal'):
        return 'missing-library' if dep_status == 'missing' else 'unresolved'
    return 'other'


def short(path, bundle_hint):
    p = path.replace('\\', '/')
    i = p.find(bundle_hint) if bundle_hint else -1
    return p[i:] if i >= 0 else p


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--doctor', required=True, action='append',
                    help='doctor JSON; repeatable. An image in a later file replaces the same image of an earlier one '
                         '(doctor --scan-bundle expands every @executable_path against the main executable, so a '
                         'helper executable is re-inspected on its own)')
    ap.add_argument('--out', required=True)
    ap.add_argument('--scan')
    ap.add_argument('--gaps')
    ap.add_argument('--dump', help='ALCHEMY_DUMP_OBJC class dump, for the dynamic class-name candidates')
    ap.add_argument('--probe', action='append', default=[])
    ap.add_argument('--sources')
    ap.add_argument('--bundle')
    ap.add_argument('--bundle-name', default='.app/', help='path fragment that starts image names in reports')
    ap.add_argument('--summary-md')
    ap.add_argument('--symbols-md')
    ap.add_argument('--areas-md')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    merged = collections.OrderedDict()
    for path in a.doctor:
        with open(path, encoding='utf-8') as f:
            for im in json.load(f).get('images', []):
                key = os.path.normcase(os.path.normpath(im.get('path', '')))
                if key in merged and path != a.doctor[0]:
                    merged[key] = im      # re-inspected on its own
                elif key not in merged and path == a.doctor[0]:
                    merged[key] = im      # only the first file adds images
    doctor = {'images': list(merged.values())}
    explicit = explicit_failures(a.sources)
    hint = a.bundle_name

    images, rows = [], []
    lib_images = collections.defaultdict(set)
    lib_status = {}
    per_symbol = {}   # (library, symbol) -> {'class', 'images', 'weak'}
    for im in doctor.get('images', []):
        name = short(im.get('path', ''), hint)
        deps = {d['identity']: d for d in im.get('dependencies') or []}
        counts = collections.Counter()
        for d in deps.values():
            prev = lib_status.get(d['identity'])
            # 'missing' wins over 'provider' only if no image resolved it.
            if prev is None or (prev == 'missing' and d['status'] != 'missing'):
                lib_status[d['identity']] = d['status']
        for imp in im.get('imports') or []:
            lib = imp.get('library') or '?'
            dep = deps.get(lib, {})
            cls = classify(imp, dep.get('status'), explicit)
            counts[cls] += 1
            lib_images[lib].add(name)
            rows.append({'image': name, 'library': lib, 'symbol': imp['symbol'], 'weak': bool(imp.get('weak')),
                         'doctor_status': imp.get('status'), 'class': cls})
            key = (lib, imp['symbol'])
            e = per_symbol.setdefault(key, {'class': cls, 'images': set(), 'weak': True})
            e['images'].add(name)
            e['weak'] = e['weak'] and bool(imp.get('weak'))
            if ORDER.index(cls) < ORDER.index(e['class']):
                e['class'] = cls
        images.append({'image': name, 'type': im.get('type'), 'error': im.get('error') or None,
                       'requirements': im.get('requirements') or [],
                       'dependencies': [{'identity': d['identity'], 'status': d['status'], 'weak': d.get('weak')}
                                        for d in im.get('dependencies') or []],
                       'imports': sum(counts.values()), 'by_class': dict(counts)})

    areas = {}
    for (lib, sym), e in per_symbol.items():
        e['area'] = area_of(lib, sym, e['images'])
        e['browser'] = any(not UPDATER.search(i) for i in e['images'])
        A = areas.setdefault(e['area'], {'area': e['area'], 'total': 0, 'browser_total': 0, 'by_class': collections.Counter(),
                                         'browser_by_class': collections.Counter(), 'gaps': []})
        A['total'] += 1
        A['by_class'][e['class']] += 1
        if e['browser']:
            A['browser_total'] += 1
            A['browser_by_class'][e['class']] += 1
        if e['class'] not in ('provided', 'guest'):
            A['gaps'].append({'library': libname(lib), 'symbol': sym, 'class': e['class'], 'browser': e['browser']})
    for A in areas.values():
        A['by_class'], A['browser_by_class'] = dict(A['by_class']), dict(A['browser_by_class'])
        A['gaps'].sort(key=lambda g: (g['library'], g['symbol']))
    libraries = {}
    for (lib, sym), e in per_symbol.items():
        L = libraries.setdefault(lib, {'library': lib, 'status': lib_status.get(lib, '?'), 'images': sorted(lib_images[lib]),
                                       'total': 0, 'by_class': collections.Counter(), 'symbols': collections.defaultdict(list)})
        L['total'] += 1
        L['by_class'][e['class']] += 1
        L['symbols'][e['class']].append(sym)
    for L in libraries.values():
        L['by_class'] = dict(L['by_class'])
        L['symbols'] = {k: sorted(v) for k, v in L['symbols'].items()}

    objc = {}
    if a.scan:
        with open(a.scan, encoding='utf-8') as f:
            scan = json.load(f)
        for im in scan.get('images', []):
            objc[im['path']] = {'classes_defined': len(im.get('classes', [])),
                                'categories': len(im.get('categories', [])),
                                'categories_on_system_classes': sorted({c['class'] for c in im.get('categories', [])
                                                                        if c.get('external') and c.get('class')}),
                                'class_imports': im.get('external_classes', []),
                                'subclassed_system_classes': im.get('subclassed_classes', []),
                                'selector_refs': len(im.get('selectors', [])),
                                'protocols': len(im.get('protocols', {}))}
    gaps = None
    if a.gaps:
        with open(a.gaps, encoding='utf-8') as f:
            gaps = json.load(f)
    strings = bundle_strings(a.bundle) if a.bundle else {}
    registered, defined = set(), set()
    if a.dump:
        with open(a.dump, encoding='utf-8') as f:
            registered = {c['name'] for c in json.load(f).get('classes', []) if c.get('native')}
    if a.scan:
        defined = {c['name'] for im in scan.get('images', []) for c in im.get('classes', [])}
    imported = {s[len('_OBJC_CLASS_$_'):] for (_l, s) in per_symbol if s.startswith('_OBJC_CLASS_$_')}
    bundle_files = set()
    if a.bundle:
        for _dp, _dn, fn in os.walk(a.bundle):
            bundle_files.update(fn)
    dynamic = {'libraries': collections.defaultdict(set), 'classes': collections.defaultdict(set)}
    for image, found in strings.items():
        for s in found['libraries']:
            if s in lib_status:
                st = 'linked:' + lib_status[s]
            elif os.path.basename(s) in bundle_files:
                st = 'in-bundle'
            else:
                st = 'not-linked'
            dynamic['libraries'][(s, st)].add(image)
        for s in found['class_names']:
            name = s.lstrip('_')
            st = ('imported' if name in imported else 'defined-in-bundle' if name in defined else
                  'registered' if name in registered else 'unregistered' if registered else 'unknown')
            dynamic['classes'][(name, st)].add(image)
    dynamic = {k: [{'name': n, 'status': st, 'images': sorted(v)} for (n, st), v in sorted(d.items())]
               for k, d in dynamic.items()}
    probes = probe_entries(a.probe)

    totals = collections.Counter(e['class'] for e in per_symbol.values())
    inventory = {
        'doctor': [os.path.abspath(p) for p in a.doctor], 'images': images,
        'totals': {'images': len(images), 'records': len(rows), 'unique_symbols': len(per_symbol),
                   'libraries': len(libraries),
                   'missing_libraries': sorted(l for l, s in lib_status.items() if s == 'missing'),
                   'by_class': dict(totals)},
        'libraries': sorted(libraries.values(), key=lambda L: L['library']),
        'areas': sorted(areas.values(), key=lambda A: A['area']),
        'objc': objc,
        'objc_gaps': gaps,
        'dynamic_name_candidates': dynamic,
        'probe': probes,
        'explicit_failure_heuristic_symbols': len(explicit),
    }
    with open(os.path.join(a.out, 'inventory.json'), 'w', encoding='utf-8') as f:
        json.dump(inventory, f, indent=1)
    with open(os.path.join(a.out, 'imports.csv'), 'w', encoding='utf-8', newline='') as f:
        w = csv.DictWriter(f, fieldnames=['image', 'library', 'symbol', 'weak', 'doctor_status', 'class'])
        w.writeheader()
        w.writerows(rows)
    with open(os.path.join(a.out, 'libraries.csv'), 'w', encoding='utf-8', newline='') as f:
        w = csv.writer(f)
        w.writerow(['library', 'library_status', 'images', 'unique_symbols'] + ORDER)
        for L in inventory['libraries']:
            w.writerow([L['library'], L['status'], len(L['images']), L['total']] + [L['by_class'].get(c, 0) for c in ORDER])
    if a.summary_md:
        write_summary(inventory, a.summary_md)
    if a.symbols_md:
        write_symbols(inventory, a.symbols_md)
    if a.areas_md:
        write_areas(inventory, a.areas_md)
    print(json.dumps(inventory['totals'], indent=1))


def libname(identity):
    base = identity.split('/')[-1]
    return base or identity


def write_summary(inv, path):
    out = ['| Library | Identity | Images | Imports | Provided | Errors / stops (heuristic) | Guest | Unsupported | '
           'Missing / unresolved | Weak-unresolved |', '| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |']
    for L in sorted(inv['libraries'], key=lambda L: (-(L['by_class'].get('missing-library', 0) +
                                                        L['by_class'].get('unresolved', 0)), L['library'])):
        b = L['by_class']
        missing = b.get('missing-library', 0) + b.get('unresolved', 0)
        out.append('| %s | %s | %d | %d | %d | %d / %d | %d | %d | %d | %d |' % (
            libname(L['library']), L['status'], len(L['images']), L['total'], b.get('provided', 0),
            b.get(ERROR, 0), b.get(STOP, 0), b.get('guest', 0), b.get('unsupported', 0), missing,
            b.get('weak-unresolved', 0)))
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out) + '\n')


def write_areas(inv, path):
    out = ['| Area | Imports | Provided | Errors / stops (heuristic) | Missing / unresolved | Weak-unresolved | '
           'Browser-tree gaps (no updater) |', '| --- | --- | --- | --- | --- | --- | --- |']
    for A in sorted(inv['areas'], key=lambda A: A['area']):
        b, bb = A['by_class'], A['browser_by_class']
        gaps = lambda c: c.get('missing-library', 0) + c.get('unresolved', 0) + c.get('unsupported', 0)
        out.append('| %s | %d | %d | %d / %d | %d | %d | %d missing, %d error/stop |' % (
            A['area'], A['total'], b.get('provided', 0) + b.get('guest', 0), b.get(ERROR, 0), b.get(STOP, 0), gaps(b),
            b.get('weak-unresolved', 0), gaps(bb), bb.get(ERROR, 0) + bb.get(STOP, 0)))
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out) + '\n')


def write_symbols(inv, path):
    out = ['# Chrome import inventory: names per library', '',
           'Generated by `scripts/alchemy_import_inventory.py` from `alchemy-fx doctor --deps --scan-bundle --json`.',
           'Every unique (library, symbol) pair imported by any arm64 Mach-O of Google Chrome.app that is not',
           'plainly provided, grouped by library identity. Classes are explained in',
           '[ALCHEMY_CHROME_GAPS.md](ALCHEMY_CHROME_GAPS.md). Do not edit by hand.', '']
    for L in sorted(inv['libraries'], key=lambda L: L['library']):
        listed = [c for c in ORDER if c not in ('provided', 'guest') and L['symbols'].get(c)]
        if not listed:
            continue
        out.append('## %s (%s)' % (L['library'], L['status']))
        out.append('')
        for c in listed:
            names = L['symbols'][c]
            out.append('**%s** (%d): %s' % (c, len(names), ' '.join('`%s`' % n for n in names)))
            out.append('')
    gaps = inv.get('objc_gaps')
    if gaps:
        out += ['# Objective-C (static metadata against the runtime class dump)', '']
        mc = gaps.get('missing_classes', [])
        out.append('**Referenced classes not registered** (%d; `*` = subclassed): %s' % (
            len(mc), ' '.join('`%s%s`' % ('*' if g.get('subclassed') else '', g['class']) for g in mc)))
        out.append('')
        sg = gaps.get('selector_gaps', [])
        out.append('**Selectors referenced (`__objc_selrefs`) that no bundle class defines and no registered host class '
                   'implements** (%d): %s' % (len(sg), ' '.join('`%s`' % g['selector'] for g in sg)))
        out.append('')
    dyn = inv.get('dynamic_name_candidates') or {}
    libs = [d for d in dyn.get('libraries', []) if not d['status'].startswith('linked')]
    if libs:
        out += ['# Library names in C strings that no image links (dlopen candidates, heuristic)', '']
        out.append(' '.join('`%s` (%s)' % (d['name'], d['status']) for d in libs))
        out.append('')
    cls = [d for d in dyn.get('classes', []) if d['status'] in ('unregistered', 'unknown')]
    if cls:
        out += ['# Class-like C strings that no image imports and the runtime does not register '
                '(NSClassFromString candidates, heuristic; includes false positives)', '']
        out.append(' '.join('`%s`' % d['name'] for d in cls))
        out.append('')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out))


if __name__ == '__main__':
    sys.exit(main())
