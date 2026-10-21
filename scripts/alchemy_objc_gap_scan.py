# SPDX-License-Identifier: MIT
"""Static Objective-C / C-import / nib gap scan of a macOS app bundle.

Reads every arm64 Mach-O image of a bundle (fat files: the arm64 slice) and
every compiled nib (NIBArchive and NSKeyedArchiver plists), then compares what
the app references with what Alchemy implements:

* the implementation side comes from the runtime: a JSON class dump written by
  alchemy(-fx) with ALCHEMY_DUMP_OBJC=<file> (see src/alchemy/objc_runtime.cpp),
* C imports come from `alchemy doctor --deps --json` (unresolved strong imports).

Usage:
  python -I alchemy_objc_gap_scan.py BUNDLE --dump DUMP.json --out OUT_DIR
      [--alchemy alchemy-fx.exe --runtime-dir DIR ... --root ROOT]
      [--probe PROBE_LOG ...] [--sources SRC_DIR]

Writes OUT_DIR/scan.json (raw per-image facts) and OUT_DIR/gaps.json (the gap
list). Only the bundle's own bytes are read; nothing is executed except the
optional doctor run.
"""
import argparse
import collections
import json
import os
import plistlib
import re
import struct
import subprocess
import sys

CPU_ARM64 = 0x0100000C
LC_SEGMENT_64, LC_SYMTAB, LC_DYSYMTAB = 0x19, 0x2, 0xB
LC_LOAD_DYLIB, LC_ID_DYLIB, LC_LOAD_WEAK_DYLIB = 0xC, 0xD, 0x80000018
LC_REEXPORT_DYLIB, LC_LAZY_LOAD_DYLIB, LC_LOAD_UPWARD_DYLIB = 0x8000001F, 0x20, 0x80000023
LC_DYLD_INFO, LC_DYLD_INFO_ONLY, LC_DYLD_CHAINED_FIXUPS = 0x22, 0x80000022, 0x80000034
DYLIB_COMMANDS = (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB, LC_LAZY_LOAD_DYLIB, LC_LOAD_UPWARD_DYLIB)


def uleb(data, at):
    value = shift = 0
    while True:
        b = data[at]
        at += 1
        value |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return value, at


def sleb(data, at):
    value = shift = 0
    while True:
        b = data[at]
        at += 1
        value |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            if b & 0x40:
                value -= 1 << shift
            return value, at


def arm64_slice(blob):
    """(offset, size) of the arm64 image in a thin or fat Mach-O, else None."""
    magic = blob[:4]
    if magic == b'\xcf\xfa\xed\xfe':
        cpu = struct.unpack_from('<I', blob, 4)[0]
        return (0, len(blob)) if cpu == CPU_ARM64 else None
    if magic in (b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf'):
        wide = magic == b'\xca\xfe\xba\xbf'
        count = struct.unpack_from('>I', blob, 4)[0]
        if count > 64:
            return None  # Java class file, not a fat Mach-O
        at = 8
        for _ in range(count):
            if wide:
                cpu, _sub, off, size, _al, _r = struct.unpack_from('>IIQQII', blob, at)
                at += 32
            else:
                cpu, _sub, off, size, _al = struct.unpack_from('>IIIII', blob, at)
                at += 20
            if cpu == CPU_ARM64:
                return off, size
    return None


class MachO:
    def __init__(self, data):
        self.d = data
        magic, cpu, _sub, self.filetype, ncmds, _size, _flags, _res = struct.unpack_from('<IIIIIIII', data, 0)
        if magic != 0xFEEDFACF or cpu != CPU_ARM64:
            raise ValueError('not an arm64 Mach-O')
        self.segments, self.sections, self.dylibs = [], [], []
        self.symtab = self.dysymtab = self.dyld_info = self.chained = None
        self.install_name = None
        at = 32
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<II', data, at)
            if cmd == LC_SEGMENT_64:
                name = data[at + 8:at + 24].rstrip(b'\0').decode()
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', data, at + 24)
                nsects = struct.unpack_from('<I', data, at + 64)[0]
                self.segments.append((name, vmaddr, vmsize, fileoff, filesize))
                s = at + 72
                for _ in range(nsects):
                    sect = data[s:s + 16].rstrip(b'\0').decode()
                    seg = data[s + 16:s + 32].rstrip(b'\0').decode()
                    addr, ssize, offset = struct.unpack_from('<QQI', data, s + 32)
                    flags, r1, r2 = struct.unpack_from('<III', data, s + 64)
                    self.sections.append({'seg': seg, 'name': sect, 'addr': addr, 'size': ssize,
                                          'offset': offset, 'flags': flags, 'r1': r1, 'r2': r2})
                    s += 80
            elif cmd == LC_SYMTAB:
                self.symtab = struct.unpack_from('<IIII', data, at + 8)
            elif cmd == LC_DYSYMTAB:
                self.dysymtab = struct.unpack_from('<18I', data, at + 8)
            elif cmd in DYLIB_COMMANDS:
                off = struct.unpack_from('<I', data, at + 8)[0]
                self.dylibs.append((data[at + off:at + size].split(b'\0')[0].decode(), cmd == LC_LOAD_WEAK_DYLIB))
            elif cmd == LC_ID_DYLIB:
                off = struct.unpack_from('<I', data, at + 8)[0]
                self.install_name = data[at + off:at + size].split(b'\0')[0].decode()
            elif cmd in (LC_DYLD_INFO, LC_DYLD_INFO_ONLY):
                self.dyld_info = struct.unpack_from('<10I', data, at + 8)
            elif cmd == LC_DYLD_CHAINED_FIXUPS:
                self.chained = struct.unpack_from('<II', data, at + 8)
            at += size
        self.base = next((s[1] for s in self.segments if s[0] == '__TEXT'), 0)
        self.fixups = {}  # vmaddr -> ('bind', symbol, ordinal) | ('rebase', target)
        self._parse_fixups()

    # -- addressing ---------------------------------------------------------
    def offset(self, addr):
        for _n, vm, vs, fo, fs in self.segments:
            if vm <= addr < vm + vs:
                rel = addr - vm
                return fo + rel if rel < fs else None
        return None

    def u32(self, addr):
        o = self.offset(addr)
        return None if o is None else struct.unpack_from('<I', self.d, o)[0]

    def i32(self, addr):
        o = self.offset(addr)
        return None if o is None else struct.unpack_from('<i', self.d, o)[0]

    def ptr(self, addr):
        """('bind', name) | ('local', vmaddr) | None for a pointer-sized slot."""
        if not addr:
            return None
        fix = self.fixups.get(addr)
        if fix:
            return ('bind', fix[1]) if fix[0] == 'bind' else ('local', fix[1])
        o = self.offset(addr)
        if o is None:
            return None
        raw = struct.unpack_from('<Q', self.d, o)[0]
        if self.chained:
            return None  # chained images keep every pointer in a fixup chain
        return ('local', raw) if raw else None

    def local(self, addr):
        p = self.ptr(addr)
        return p[1] if p and p[0] == 'local' else 0

    def cstr(self, addr):
        o = self.offset(addr)
        if o is None:
            return None
        end = self.d.find(b'\0', o)
        return self.d[o:end].decode('utf-8', 'replace')

    def section(self, name, seg=None):
        return [s for s in self.sections if s['name'] == name and (seg is None or s['seg'] == seg)]

    # -- fixups -------------------------------------------------------------
    def _parse_fixups(self):
        if self.chained:
            self._chained(*self.chained)
        elif self.dyld_info:
            info = self.dyld_info
            for off, size, lazy in ((info[2], info[3], False), (info[4], info[5], False), (info[6], info[7], True)):
                if size:
                    self._bind_opcodes(self.d[off:off + size], lazy)

    def _chained(self, off, size):
        d = self.d
        _ver, starts, imports, symbols, count, fmt, _sfmt = struct.unpack_from('<7I', d, off)
        names = []
        at = off + imports
        for _ in range(count):
            if fmt == 1:
                v = struct.unpack_from('<I', d, at)[0]
                at += 4
                ordinal, name = v & 0xFF, v >> 9
            elif fmt == 2:
                v = struct.unpack_from('<I', d, at)[0]
                at += 8
                ordinal, name = v & 0xFF, v >> 9
            else:
                v = struct.unpack_from('<Q', d, at)[0]
                at += 16
                ordinal, name = v & 0xFFFF, v >> 32
            e = d.find(b'\0', off + symbols + name)
            names.append((d[off + symbols + name:e].decode(), ordinal))
        seg_count = struct.unpack_from('<I', d, off + starts)[0]
        for i in range(seg_count):
            info = struct.unpack_from('<I', d, off + starts + 4 + 4 * i)[0]
            if not info:
                continue
            s = off + starts + info
            _sz, page_size, pfmt, seg_off, _maxp, pages = struct.unpack_from('<IHHQIH', d, s)
            seg = next((x for x in self.segments if x[1] - self.base == seg_off), None)
            if not seg:
                continue
            stride = 4 if pfmt in (2, 6, 1, 9, 12) else 4
            for p in range(pages):
                start = struct.unpack_from('<H', d, s + 22 + 2 * p)[0]
                if start == 0xFFFF:
                    continue
                vm = seg[1] + p * page_size + start
                while True:
                    fo = self.offset(vm)
                    v = struct.unpack_from('<Q', d, fo)[0]
                    nxt = (v >> 51) & 0xFFF
                    if v >> 63:
                        index = v & 0xFFFFFF
                        if index < len(names):
                            self.fixups[vm] = ('bind', names[index][0], names[index][1])
                    else:
                        target = v & 0xFFFFFFFFF
                        if pfmt == 6:
                            target += self.base
                        self.fixups[vm] = ('rebase', target)
                    if not nxt:
                        break
                    vm += nxt * stride

    def _bind_opcodes(self, ops, lazy):
        segs = self.segments
        at, ordinal, name, seg, off = 0, 0, None, 0, 0
        while at < len(ops):
            b = ops[at]
            at += 1
            op, imm = b & 0xF0, b & 0x0F
            if op == 0x00:
                if not lazy:
                    break
            elif op == 0x10:
                ordinal = imm
            elif op == 0x20:
                ordinal, at = uleb(ops, at)
            elif op == 0x30:
                ordinal = (imm | 0xF0) - 0x100 if imm else 0
            elif op == 0x40:
                e = ops.index(b'\0', at)
                name = ops[at:e].decode()
                at = e + 1
            elif op == 0x50:
                pass
            elif op == 0x60:
                _, at = sleb(ops, at)
            elif op == 0x70:
                seg = imm
                off, at = uleb(ops, at)
            elif op == 0x80:
                v, at = uleb(ops, at)
                off = (off + v) & 0xFFFFFFFFFFFFFFFF
            elif op in (0x90, 0xA0, 0xB0, 0xC0):
                times, skip = 1, 0
                if op == 0xC0:
                    times, at = uleb(ops, at)
                    skip, at = uleb(ops, at)
                for _ in range(times):
                    self.fixups[segs[seg][1] + off] = ('bind', name, ordinal)
                    off += 8 + skip
                if op == 0xA0:
                    v, at = uleb(ops, at)
                    off = (off + v) & 0xFFFFFFFFFFFFFFFF
                elif op == 0xB0:
                    off += imm * 8
            elif op == 0xD0:
                if imm == 0:
                    _, at = uleb(ops, at)
            else:
                break

    # -- symbols ------------------------------------------------------------
    def symbols(self):
        """(undefined [(name, library, weak_ref)], defined_external [name])."""
        if not self.symtab:
            return [], []
        symoff, nsyms, stroff, _strsize = self.symtab
        und, defined = [], []
        for i in range(nsyms):
            strx, ntype, _sect, desc, _value = struct.unpack_from('<IBBHQ', self.d, symoff + 16 * i)
            if ntype & 0xE0:
                continue  # debugging entry
            e = self.d.find(b'\0', stroff + strx)
            name = self.d[stroff + strx:e].decode('utf-8', 'replace')
            if (ntype & 0x0E) == 0 and ntype & 0x01:
                ordinal = (desc >> 8) & 0xFF
                lib = (self.dylibs[ordinal - 1][0] if 0 < ordinal <= len(self.dylibs)
                       else {0: 'self', 0xFE: 'executable', 0xFF: 'flat'}.get(ordinal, '?'))
                und.append((name, lib, bool(desc & 0x40)))
            elif (ntype & 0x0E) == 0x0E and ntype & 0x01:
                defined.append(name)
        return und, defined


# -- Objective-C metadata ----------------------------------------------------
def class_name_of(m, p):
    """Name of the class a pointer names: a bound _OBJC_CLASS_$_X or a local class."""
    if not p:
        return None, None
    if p[0] == 'bind':
        sym = p[1]
        for pre in ('_OBJC_CLASS_$_', '_OBJC_METACLASS_$_'):
            if sym.startswith(pre):
                return sym[len(pre):], 'external'
        return sym, 'external-symbol'
    ro = m.local(p[1] + 32) & 0x00007FFFFFFFFFF8
    if not ro:
        return None, None
    return m.cstr(m.local(ro + 24)), 'local'


def method_list(m, addr):
    out = []
    if not addr:
        return out
    flags, count = m.u32(addr), m.u32(addr + 4)
    if flags is None or count is None or count > 100000:
        return out
    entsize = flags & 0x0000FFFC
    relative = bool(flags & 0x80000000)
    at = addr + 8
    for _ in range(count):
        if relative:
            rel = m.i32(at)
            selref = at + rel
            name = m.cstr(m.local(selref)) if not flags & 0x40000000 else m.cstr(selref)
            imp_rel = m.i32(at + 8)
            imp = at + 8 + imp_rel
        else:
            name = m.cstr(m.local(at))
            imp = m.local(at + 16)
        if name:
            out.append((name, imp))
        at += entsize or (12 if relative else 24)
    return out


def read_class(m, addr):
    ro = m.local(addr + 32) & 0x00007FFFFFFFFFF8
    if not ro:
        return None
    name = m.cstr(m.local(ro + 24))
    sup_name, sup_kind = class_name_of(m, m.ptr(addr + 8))
    meta = m.local(addr)
    meta_ro = m.local(meta + 32) & 0x00007FFFFFFFFFF8 if meta else 0
    inst = method_list(m, m.local(ro + 32))
    cls = method_list(m, m.local(meta_ro + 32)) if meta_ro else []
    return {'name': name, 'super': sup_name, 'super_external': sup_kind == 'external',
            'instance': inst, 'class': cls, 'swift': bool(m.local(addr + 32) & 3)}


def scan_objc(m):
    classes, categories = [], []
    for sect in m.section('__objc_classlist'):
        for i in range(sect['size'] // 8):
            a = m.local(sect['addr'] + 8 * i)
            if a:
                c = read_class(m, a)
                if c:
                    classes.append(c)
    for sect in m.section('__objc_catlist'):
        for i in range(sect['size'] // 8):
            a = m.local(sect['addr'] + 8 * i)
            if not a:
                continue
            cname, kind = class_name_of(m, m.ptr(a + 8))
            categories.append({'name': m.cstr(m.local(a)), 'class': cname, 'external': kind == 'external',
                               'instance': method_list(m, m.local(a + 16)),
                               'class_methods': method_list(m, m.local(a + 24))})
    protocols = {}
    for sect in m.section('__objc_protolist'):
        for i in range(sect['size'] // 8):
            a = m.local(sect['addr'] + 8 * i)
            name = m.cstr(m.local(a + 8)) if a else None
            if not name:
                continue
            entry = protocols.setdefault(name, {'required': set(), 'optional': set()})
            for off, kind in ((24, 'required'), (32, 'required'), (40, 'optional'), (48, 'optional')):
                entry[kind].update(n for n, _ in method_list(m, m.local(a + off)))
    classrefs, superrefs = {}, {}
    for key, store in (('__objc_classrefs', classrefs), ('__objc_superrefs', superrefs)):
        for sect in m.section(key):
            for i in range(sect['size'] // 8):
                slot = sect['addr'] + 8 * i
                store[slot] = class_name_of(m, m.ptr(slot))
    selrefs = {}
    for sect in m.section('__objc_selrefs'):
        for i in range(sect['size'] // 8):
            slot = sect['addr'] + 8 * i
            name = m.cstr(m.local(slot))
            if name:
                selrefs[slot] = name
    return classes, categories, classrefs, superrefs, selrefs, protocols


# -- nibs and keyed archives -------------------------------------------------
def nib_archive(blob):
    if not blob.startswith(b'NIBArchive'):
        return None
    hdr = struct.unpack_from('<10I', blob, 10)
    _fmt, _cv, ocount, ooff, kcount, koff, vcount, voff, ccount, coff = hdr

    def var(at):
        v = shift = 0
        while True:
            b = blob[at]
            at += 1
            v |= (b & 0x7F) << shift
            shift += 7
            if b & 0x80:
                return v, at
    classes, at = [], coff
    for _ in range(ccount):
        ln, at = var(at)
        extra, at = var(at)
        at += 4 * extra
        classes.append(blob[at:at + ln].rstrip(b'\0').decode('utf-8', 'replace'))
        at += ln
    keys, at = [], koff
    for _ in range(kcount):
        ln, at = var(at)
        keys.append(blob[at:at + ln].decode('utf-8', 'replace'))
        at += ln
    values, at = [], voff
    sizes = {0: 1, 1: 2, 2: 4, 3: 8, 4: 0, 5: 0, 6: 4, 7: 8, 9: 0, 10: 4}
    for _ in range(vcount):
        k, at = var(at)
        t = blob[at]
        at += 1
        payload = None
        if t == 8:
            ln, at = var(at)
            payload = blob[at:at + ln]
            at += ln
        elif t == 10:
            payload = struct.unpack_from('<I', blob, at)[0]
            at += 4
        else:
            if t in (0, 1, 2, 3):
                payload = int.from_bytes(blob[at:at + sizes[t]], 'little', signed=True)
            at += sizes.get(t, 0)
        values.append((k, t, payload))
    objects, at = [], ooff
    for _ in range(ocount):
        c, at = var(at)
        first, at = var(at)
        n, at = var(at)
        objects.append((c, first, n))
    result = []
    for c, first, n in objects:
        name = classes[c]
        ks = [keys[values[j][0]] for j in range(first, first + n)]
        strs = {}
        for j in range(first, first + n):
            k, t, p = values[j]
            if t == 8 and isinstance(p, (bytes, bytearray)):
                strs[keys[k]] = p
        result.append({'class': name, 'keys': ks, 'data': strs, 'values': [(keys[values[j][0]],) + values[j][1:]
                                                                          for j in range(first, first + n)]})
    return {'objects': result}


def nib_details(parsed):
    """Classes, keys per class, custom classes, runtime attributes and bindings of a NIBArchive."""
    objs = parsed['objects']
    out = {'classes': collections.Counter(), 'keys': collections.defaultdict(set),
           'custom_classes': set(), 'runtime_attributes': set(), 'bindings': set(), 'connector_classes': set()}

    def deref(value):
        return objs[value] if isinstance(value, int) and 0 <= value < len(objs) else None

    def string_of(obj):
        if not obj:
            return None
        for k, t, p in obj['values']:
            if k == 'NS.bytes' and t == 8:
                return bytes(p).decode('utf-8', 'replace')
        return None

    for o in objs:
        out['classes'][o['class']] += 1
        for k in o['keys']:
            if k != 'UINibEncoderEmptyKey':
                out['keys'][o['class']].add(k)
        for k, t, p in o['values']:
            if k in ('NSClassName', 'NSOriginalClassName') and t == 10:
                s = string_of(deref(p))
                if s:
                    out['custom_classes'].add(s)
            if k == 'NSKeyPaths' and t == 10 and o['class'] == 'NSIBUserDefinedRuntimeAttributesConnector':
                arr = deref(p)
                for k2, t2, p2 in (arr['values'] if arr else []):
                    s = string_of(deref(p2)) if t2 == 10 else None
                    if s:
                        out['runtime_attributes'].add(s)
            if k == 'NSBinding' and t == 10:
                s = string_of(deref(p))
                if s:
                    out['bindings'].add(s)
        if o['class'].endswith('Connector'):
            out['connector_classes'].add(o['class'])
    return out


def keyed_archive_classes(path):
    try:
        with open(path, 'rb') as f:
            pl = plistlib.load(f)
    except Exception:
        return None
    if not isinstance(pl, dict) or pl.get('$archiver') not in ('NSKeyedArchiver',):
        return None
    out = {'classes': collections.Counter(), 'keys': collections.defaultdict(set)}
    objs = pl.get('$objects', [])

    def cname(obj):
        c = obj.get('$class')
        if isinstance(c, plistlib.UID) and c.data < len(objs):
            meta = objs[c.data]
            if isinstance(meta, dict):
                return meta.get('$classname')
        return None
    for o in objs:
        if isinstance(o, dict) and '$class' in o:
            n = cname(o)
            if n:
                out['classes'][n] += 1
                out['keys'][n].update(k for k in o if k != '$class')
    return out


# -- bundle walk -------------------------------------------------------------
def walk_bundle(bundle):
    machos, nibs, archives = [], [], []
    for dp, _dn, fn in os.walk(bundle):
        for n in fn:
            p = os.path.join(dp, n)
            try:
                if os.path.getsize(p) < 16:
                    continue
                with open(p, 'rb') as f:
                    head = f.read(16)
            except OSError:
                continue
            if head[:4] in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf'):
                machos.append(p)
            elif head.startswith(b'NIBArchive'):
                nibs.append(p)
            elif head.startswith(b'bplist00') and (n.endswith('.nib') or n.endswith('.plist') or '.storyboardc' in p):
                archives.append(p)
    return sorted(machos), sorted(nibs), sorted(archives)


def rel(p, base):
    return os.path.relpath(p, base).replace('\\', '/')


def scan_bundle(bundle):
    machos, nibs, archives = walk_bundle(bundle)
    images = []
    seen = set()
    for p in machos:
        with open(p, 'rb') as f:
            blob = f.read()
        sl = arm64_slice(blob)
        if not sl:
            continue
        data = blob[sl[0]:sl[0] + sl[1]]
        key = hash(data)
        if key in seen:
            continue  # hard-linked framework alias
        seen.add(key)
        try:
            m = MachO(data)
        except Exception as e:
            images.append({'path': rel(p, bundle), 'error': str(e)})
            continue
        classes, categories, classrefs, superrefs, selrefs, protocols = scan_objc(m)
        und, defined = m.symbols()
        ext_classes = sorted({s[len('_OBJC_CLASS_$_'):] for s, _l, _w in und if s.startswith('_OBJC_CLASS_$_')})
        ext_meta = sorted({s[len('_OBJC_METACLASS_$_'):] for s, _l, _w in und if s.startswith('_OBJC_METACLASS_$_')})
        images.append({
            'path': rel(p, bundle), 'install_name': m.install_name, 'dylibs': m.dylibs,
            'classes': [{'name': c['name'], 'super': c['super'], 'super_external': c['super_external'],
                         'swift': c['swift'], 'instance': sorted({n for n, _ in c['instance']}),
                         'class': sorted({n for n, _ in c['class']})} for c in classes],
            'categories': [{'name': c['name'], 'class': c['class'], 'external': c['external'],
                            'instance': sorted({n for n, _ in c['instance']}),
                            'class_methods': sorted({n for n, _ in c['class_methods']})} for c in categories],
            'external_classes': ext_classes, 'subclassed_classes': ext_meta,
            'classrefs': sorted({v[0] for v in classrefs.values() if v[0] and v[1] == 'external'}),
            'superrefs': sorted({v[0] for v in superrefs.values() if v[0] and v[1] == 'external'}),
            'selectors': sorted(set(selrefs.values())),
            'protocols': {n: {k: sorted(v) for k, v in p.items()} for n, p in sorted(protocols.items())},
            'imports': [{'symbol': s, 'library': l, 'weak': w} for s, l, w in und],
            'exports': len(defined),
        })
        sys.stderr.write('image %-70s classes %4d cats %3d selrefs %5d\n' % (
            rel(p, bundle)[-70:], len(classes), len(categories), len(selrefs)))
    nib_records = []
    for p in nibs:
        with open(p, 'rb') as f:
            blob = f.read()
        try:
            parsed = nib_archive(blob)
        except Exception as e:
            nib_records.append({'path': rel(p, bundle), 'error': str(e)})
            continue
        d = nib_details(parsed)
        nib_records.append({'path': rel(p, bundle), 'format': 'NIBArchive', 'classes': dict(d['classes']),
                            'keys': {c: sorted(k) for c, k in d['keys'].items()},
                            'custom_classes': sorted(d['custom_classes']),
                            'runtime_attributes': sorted(d['runtime_attributes']),
                            'bindings': sorted(d['bindings'])})
    for p in archives:
        d = keyed_archive_classes(p)
        if d:
            nib_records.append({'path': rel(p, bundle), 'format': 'NSKeyedArchiver', 'classes': dict(d['classes']),
                                'keys': {c: sorted(k) for c, k in d['keys'].items()},
                                'custom_classes': [], 'runtime_attributes': [], 'bindings': []})
    return {'bundle': os.path.basename(os.path.normpath(bundle)), 'images': images, 'nibs': nib_records}


# -- implementation side -----------------------------------------------------
class Host:
    """The runtime class dump: native (host) classes with their selectors."""

    def __init__(self, path, placeholders=()):
        with open(path, encoding='utf-8') as f:
            dump = json.load(f)
        # Classes a probe run registered as empty stand-ins for missing ones.
        for c in dump['classes']:
            if c['name'] in placeholders:
                c['native'] = False
        self.classes = {c['name']: c for c in dump['classes']}
        self.children = collections.defaultdict(list)
        for c in dump['classes']:
            if c.get('super'):
                self.children[c['super']].append(c['name'])
        self.all_instance = set()
        self.all_class = set()
        for c in dump['classes']:
            if c.get('native'):
                self.all_instance.update(c.get('instance', []))
                self.all_class.update(c.get('class', []))

    def native(self, name):
        c = self.classes.get(name)
        return bool(c and c.get('native'))

    def chain(self, name):
        seen = 0
        while name and name in self.classes and seen < 64:
            yield self.classes[name]
            name = self.classes[name].get('super')
            seen += 1

    def implements(self, name, sel, meta, subclasses=False):
        key = 'class' if meta else 'instance'
        for c in self.chain(name):
            if sel in c.get(key, ()) or sel in c.get('guest_' + key, ()):
                return True
        if meta:  # class methods fall back to the root class's instance methods
            for c in self.chain(name):
                if not c.get('super') and sel in c.get('instance', ()):
                    return True
        if subclasses:
            stack = list(self.children.get(name, ()))
            while stack:
                n = stack.pop()
                c = self.classes.get(n, {})
                if sel in c.get(key, ()):
                    return True
                stack.extend(self.children.get(n, ()))
        return False

    def decodes(self, name):
        for c in self.chain(name):
            if 'initWithCoder:' in c.get('instance', ()):
                return True
        return False


# -- attribution -------------------------------------------------------------
def load_attribution(path):
    if not path or not os.path.exists(path):
        return {}, []
    with open(path, encoding='utf-8') as f:
        data = json.load(f)
    rules = [(re.compile(r['pattern']), r['class']) for r in data.get('rules', [])]
    return data.get('selectors', {}), rules


# Protocols whose methods system objects implement and the app sends to them
# (the rest are delegate/data-source protocols: the host calls the app).
HOST_SIDE_PROTOCOLS = {
    'NSAccessibility': 'NSView (NSAccessibility)', 'NSAccessibilityElement': 'NSAccessibilityElement',
    'NSAccessibilityTable': 'NSTableView (NSAccessibility)', 'NSAccessibilityRow': 'NSTableRowView (NSAccessibility)',
    'NSAccessibilityOutline': 'NSOutlineView (NSAccessibility)', 'NSAccessibilityButton': 'NSButton (NSAccessibility)',
    'NSAccessibilityGroup': 'NSView (NSAccessibility)', 'NSAccessibilityList': 'NSView (NSAccessibility)',
    'NSAccessibilityStaticText': 'NSView (NSAccessibility)', 'NSAccessibilityNavigableStaticText': 'NSView (NSAccessibility)',
    'NSAccessibilitySlider': 'NSSlider (NSAccessibility)', 'NSAccessibilityImage': 'NSImageView (NSAccessibility)',
    'NSAccessibilityCheckBox': 'NSButton (NSAccessibility)', 'NSAccessibilityRadioButton': 'NSButton (NSAccessibility)',
    'NSAccessibilityLayoutArea': 'NSView (NSAccessibility)', 'NSAccessibilityLayoutItem': 'NSView (NSAccessibility)',
    'NSAccessibilityStepper': 'NSStepper (NSAccessibility)', 'NSAccessibilitySwitch': 'NSSwitch (NSAccessibility)',
    'NSAccessibilityProgressIndicator': 'NSProgressIndicator (NSAccessibility)',
    'NSDraggingInfo': 'NSDraggingInfo (drag session object)', 'NSAnimatablePropertyContainer': 'NSView (animator)',
    'NSLocking': 'NSLock', 'CAMediaTiming': 'CAAnimation', 'CAAction': 'CAAnimation',
    'NSTextInputClient': 'NSTextView', 'NSStandardKeyBindingResponding': 'NSResponder', 'NSTextInput': 'NSTextView',
    'NSUserInterfaceItemIdentification': 'NSView', 'NSAppearanceCustomization': 'NSView/NSWindow',
    'NSTextLayoutOrientationProvider': 'NSTextContainer', 'NSUserInterfaceCompression': 'NSView',
    'NSFilePromiseProviderDelegate': None, 'NSDraggingSource': None, 'NSDraggingDestination': None,
}
CALLBACK_SUFFIX = re.compile(r'(Delegate|DataSource|Delegating|Observer|Client|Driver|Validation|Validations|Restoration|'
                             r'Restoring|Presenter|Source|Destination|Coding|Copying|Handler|Receiving|Target|Responder|'
                             r'Provider|Item|Exporting|Interface|Protocol|Canvas|View|Controller|Window)$')


def protocol_owner(sel, sel_protocols):
    """('callback', protocol) when only delegate-style protocols declare `sel`, ('host', owner) for
    a system-implemented protocol, else (None, None)."""
    names = sel_protocols.get(sel)
    if not names:
        return None, None
    for n in sorted(names):
        owner = HOST_SIDE_PROTOCOLS.get(n, '')
        if owner:
            return 'host', owner
    for n in sorted(names):
        if n.startswith(('MTL', 'CI', 'AV', 'CA')) and not CALLBACK_SUFFIX.search(n):
            return 'host', n
    return 'callback', sorted(names)[0]


# Implementation groups (disjoint file ownership; see docs/ALCHEMY_QLAB_GAPS.md).
GROUP_RULES = [
    ('G6', r'^(CA|CI|MTK|MTL|GLK|WK|Web)|^NSGradient|^NSShadow|^NSAffineTransform|^NSImage(SymbolConfiguration)?\b|'
           r'^NSColor\b|^NSGraphicsContext'),
    ('G5', r'^(AV|AU)|^AudioToolbox|^CoreMIDI'),
    ('G2', r'^NS(Exception|Lock|Invocation|MethodSignature|BlockOperation|Operation|NotificationQueue|Thread|UndoManager|Task|Pipe|'
           r'FileHandle|InputStream|FileManager|DirectoryEnumerator|Bundle|ProcessInfo|XPC|Script|AppleScript|AppleEventDescriptor|'
           r'PropertySpecifier|UniqueIDSpecifier|HTTPCookieStorage|ArrayController|ObjectController|NibBindingConnector|'
           r'DeleteCommand|MoveCommand|UserDefaultsController|Object\b|'
           r'_NSManagedProxy)|^OS_dispatch_queue|^NSObject \((KVO|KVC|bindings|NSMethodSignature)\)|^_NSManagedProxy|^OSLog'),
    ('G1', r'^NS(KeyedArchiver|KeyedUnarchiver|Data|MutableData|String|MutableString|Scanner|RegularExpression|Predicate|SortDescriptor|'
           r'DecimalNumber|ByteCountFormatter|ListFormatter|NumberFormatter|Formatter|Calendar|DateComponents|Date\b|Locale|XML|'
           r'CharacterSet|Array|MutableArray|Dictionary|MutableDictionary|Set|MutableSet|IndexSet|IndexPath|HashTable|MapTable|'
           r'PointerArray|URL|Number|MutableAttributedString|AttributedString|Value)|^UTType|^NSDate/'),
    ('G3', r'^NS(Button|Segment|SearchField|PathC|PathComponentCell|DatePicker|ColorWell|Switch|Combo|Slider|Scroller|TextView|'
           r'LayoutManager|TextStorage|TextTab|TextViewSharedData|TextContainer|TextInputContext|FontManager|FontPanel|TableView|'
           r'TableRowView|OutlineView|TreeNode|RulerView|CollectionView|ProgressIndicator|Progress\b|Stepper|PopUpButton|Menu|'
           r'TextField|SecureTextField|AccessibilityElement|Touch|_NSCollectionViewCore|ScrollView|ClipView|Box|ImageView|ImageCell|'
           r'TableCellView|TableColumn|TableHeader|PanGestureRecognizer|Font\b|FontDescriptor|ParagraphStyle)|^_NSCollectionViewCore|'
           r'^_NSCornerView|NSAccessibility'),
    ('G4', r'^NS(Window|Popover|Alert|OpenPanel|SavePanel|Document|Application|Workspace|LayoutConstraint|LayoutAnchor|StackView|'
           r'GridView|GridRow|GridColumn|GridCell|View|AnimationContext|Toolbar|Pasteboard|Dragging|FilePromise|Event|TabView|'
           r'Responder|SplitView|ViewController|StackViewContainer|Nib|IB|ClassSwapper|Custom|LocalizableString|ColorSpace|Appearance|'
           r'Cursor)|^IBClassReference|Auto Layout'),
]
C_GROUPS = [
    ('G5', ('AudioToolbox', 'CoreMIDI', 'CoreMedia', 'AVFoundation', 'CoreVideo', 'AudioUnit', 'Accelerate', 'VideoToolbox',
            'CoreAudio', 'CoreAudioKit')),
    ('G6', ('OpenGL', 'CryptoKit', 'libswift', 'QuartzCore', 'CoreImage', 'CoreGraphics', 'Metal', 'MetalKit', 'GLKit',
            'IOSurface', 'CoreText', 'ImageIO')),
    ('G4', ('AppKit',)),
    ('G1', ('UniformTypeIdentifiers', 'CoreServices')),
    ('G2', ('libSystem', 'CoreFoundation', 'Foundation', 'ServiceManagement', 'IOKit', 'libobjc', 'Security', 'CFNetwork',
            'SystemConfiguration', 'DiskArbitration', 'libcompression')),
]


def group_of(owner):
    for gid, rx in GROUP_RULES:
        if re.search(rx, owner or ''):
            return gid
    return '?'


def c_group(library):
    name = library.split('/')[-1]
    for gid, names in C_GROUPS:
        if any(name.startswith(n) for n in names):
            return gid
    return '?'


HELPERS = ('Autoupdate', 'Updater.app', 'Downloader.xpc', 'Installer.xpc')


def helper_only(images):
    return bool(images) and all(any(h in i for h in HELPERS) for i in images)


def attribute(sel, exact, rules, calls_for):
    if calls_for:
        return calls_for, 'given'
    if sel in exact:
        return exact[sel], 'table'
    for rx, cls in rules:
        if rx.search(sel):
            return cls, 'rule'
    return 'unknown', 'none'


def doctor_imports(alchemy, runtime_dirs, root, bundle):
    cmd = [alchemy, 'doctor', '--deps', '--json']
    for d in runtime_dirs:
        cmd += ['--runtime-dir', d]
    if root:
        cmd += ['--root', root]
    cmd.append(bundle)
    env = dict(os.environ, ALCHEMY_GUI_NO_ACTIVATE='1')
    env.pop('ALCHEMY_PROBE_MISSING', None)
    r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace', env=env)
    try:
        return json.loads(r.stdout)
    except ValueError:
        sys.stderr.write('doctor failed: %s\n' % (r.stderr or r.stdout)[:400])
        return None


def source_strings(src):
    """Quoted string literals in Alchemy's sources (an approximation of the nib keys it reads)."""
    found = set()
    if not src:
        return found
    rx = re.compile(r'"((?:NS|IB|UI)[A-Za-z0-9_.]+)"')
    for dp, _dn, fn in os.walk(src):
        for n in fn:
            if n.endswith(('.cpp', '.hpp', '.inc')):
                with open(os.path.join(dp, n), encoding='utf-8', errors='replace') as f:
                    found.update(rx.findall(f.read()))
    return found


def nib_loader_names(src):
    """(classes the nib loader names, classes it rejects explicitly) from src/alchemy/darwin/appkit/nib*.cpp."""
    named, rejected = set(), set()
    if not src:
        return named, rejected
    folder = os.path.join(src, 'darwin', 'appkit')
    for n in os.listdir(folder) if os.path.isdir(folder) else []:
        if n.startswith('nib') and n.endswith('.cpp'):
            with open(os.path.join(folder, n), encoding='utf-8', errors='replace') as f:
                text = f.read()
            named.update(re.findall(r'"(NS[A-Za-z]+|IB[A-Za-z]+|_NS[A-Za-z]+)"', text))
            for msg in re.findall(r'unsupported\("nib ([^"]+)"', text):
                rejected.update(re.findall(r'\b(NS[A-Za-z]+)\b', msg))
    return named, rejected


def parse_probe(paths):
    """First-seen ordered (kind, name) entries from `[alchemy probe] ...` lines."""
    out, seen = [], set()
    rx_msg = re.compile(r'\[alchemy probe\] message ([+-])\[(\S+) (.+)\]')
    rx_sym = re.compile(r'\[alchemy probe\] (?:missing|symbol|call)[^\]]*?\b(_\S+)')
    for path in paths:
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                mm = rx_msg.search(line)
                if mm:
                    key = ('message', mm.group(1), mm.group(2), mm.group(3).strip())
                elif 'alchemy probe' in line:
                    ms = rx_sym.search(line)
                    if not ms:
                        continue
                    key = ('symbol', '', '', ms.group(1))
                else:
                    continue
                if key not in seen:
                    seen.add(key)
                    out.append(key)
    return out


def build_gaps(scan, host, doctor, attribution, src_strings, probe, nib_names=(set(), set())):
    exact, rules = attribution
    guest_classes, guest_inst, guest_cls = {}, set(), set()
    for im in scan['images']:
        for c in im.get('classes', []):
            guest_classes[c['name']] = c
            guest_inst.update(c['instance'])
            guest_cls.update(c['class'])
        for c in im.get('categories', []):
            guest_inst.update(c['instance'])
            guest_cls.update(c['class_methods'])
    guest_all = guest_inst | guest_cls
    sel_protocols = collections.defaultdict(set)
    for im in scan['images']:
        for name, p in im.get('protocols', {}).items():
            for sel in p['required'] + p['optional']:
                sel_protocols[sel].add(name)

    def first_host(name):
        """The first non-guest ancestor of a (guest) class."""
        seen = 0
        while name in guest_classes and seen < 64:
            name = guest_classes[name]['super']
            seen += 1
        return name

    # (a) classes
    referenced = collections.defaultdict(set)
    subclassed = set()
    for im in scan['images']:
        for n in im.get('external_classes', []):
            referenced[n].add(im['path'])
        for n in im.get('subclassed_classes', []):
            subclassed.add(n)
            referenced[n].add(im['path'])
        for c in im.get('categories', []):
            if c['external'] and c['class']:
                referenced[c['class']].add(im['path'])
    code_refs = set(referenced)
    for nib in scan['nibs']:
        for n in list(nib.get('classes', {})) + nib.get('custom_classes', []):
            if n not in guest_classes:
                referenced[n].add(nib['path'])
    # Nib-only classes the nib loader decodes by name (NSIBObjectData, connectors, ...).
    nib_internal = {n for n in referenced if n not in code_refs and n in src_strings and not host.native(n)}
    class_gaps = []
    for n in sorted(referenced):
        if n in guest_classes or host.native(n) or n.startswith('_TtCs') or n in nib_internal:
            continue
        class_gaps.append({'class': n, 'subclassed': n in subclassed, 'group': group_of(n),
                           'nib_only': n not in code_refs, 'helper_only': helper_only(referenced[n]),
                           'sources': sorted(referenced[n])[:6]})
    missing_classes = {g['class'] for g in class_gaps}

    # (b) selectors: referenced, not defined by guest code, not implemented by any host class.
    sel_refs = collections.defaultdict(set)
    for im in scan['images']:
        for s in im.get('selectors', []):
            sel_refs[s].add(im['path'])
    selector_gaps, callbacks = [], []
    for s in sorted(sel_refs):
        if s in guest_all or s in host.all_instance or s in host.all_class or s.startswith('.cxx'):
            continue
        owner = None
        kind, proto = protocol_owner(s, sel_protocols)
        if kind == 'callback' and not owner:
            callbacks.append({'selector': s, 'protocol': proto})
            continue
        if kind == 'host' and not owner:
            owner = proto
        cls, how = attribute(s, exact, rules, owner)
        if kind == 'host' and how == 'given':
            how = 'protocol'
        if cls == 'guest':
            callbacks.append({'selector': s, 'protocol': 'guest informal protocol'})
            continue
        selector_gaps.append({'selector': s, 'owner': cls, 'attribution': how, 'group': group_of(cls),
                              'helper_only': helper_only(sel_refs[s]), 'images': sorted(sel_refs[s])[:4]})
    # (c) C imports
    c_gaps = []
    if doctor:
        seen = {}
        for im in doctor.get('images', []):
            for i in im.get('imports') or []:
                if i.get('weak') or i.get('status') not in ('unresolved', 'invalid-ordinal'):
                    continue
                sym = i['symbol']
                if sym.startswith(('_OBJC_CLASS_$_', '_OBJC_METACLASS_$_')):
                    continue
                e = seen.setdefault((i.get('library') or '?', sym), {'library': i.get('library') or '?', 'symbol': sym,
                                                                   'images': set()})
                e['images'].add(os.path.basename(im.get('path', '')))
        c_gaps = [dict(v, images=sorted(v['images']), group=c_group(v['library'])) for _k, v in sorted(seen.items())]
    # (d) nibs
    nib_class_use = collections.defaultdict(set)
    nib_keys = collections.defaultdict(set)
    runtime_attrs, bindings = set(), set()
    for nib in scan['nibs']:
        for c in nib.get('classes', {}):
            nib_class_use[c].add(nib['path'])
        for c, ks in nib.get('keys', {}).items():
            nib_keys[c].update(ks)
        runtime_attrs.update(nib.get('runtime_attributes', []))
        bindings.update(nib.get('bindings', []))
    nib_gaps = []
    for c in sorted(nib_class_use):
        host_class = c if c not in guest_classes else first_host(c)
        if c in guest_classes:
            # Guest subclasses decode through their first host ancestor.
            status = 'ok' if host.decodes(host_class) or 'initWithCoder:' in guest_classes[c]['instance'] else \
                'no-initWithCoder'
        elif c in nib_names[1]:
            status = 'rejected-by-nib-loader'
        elif c in nib_internal or (c in nib_names[0] and not host.decodes(c)):
            status = 'ok'
        elif not host.native(c):
            status = 'class-missing'
        elif not host.decodes(c):
            status = 'no-initWithCoder'
        else:
            status = 'ok'
        unknown_keys = sorted(k for k in nib_keys.get(c, ()) if k.startswith(('NS', 'IB')) and k not in src_strings)
        if status != 'ok' or unknown_keys:
            nib_gaps.append({'class': c, 'status': status, 'group': group_of(c if c not in guest_classes else host_class), 'nibs': len(nib_class_use[c]),
                             'unread_keys': unknown_keys, 'examples': sorted(nib_class_use[c])[:3]})
    # Probe (dynamic) results
    dynamic = []
    for order, (kind, sign, cls, name) in enumerate(probe, 1):
        dynamic.append({'order': order, 'kind': kind, 'class': cls, 'group': group_of(cls), 'selector': name if kind == 'message' else None,
                        'symbol': name if kind == 'symbol' else None, 'message_kind': sign})
    return {
        'totals': {'missing_classes': len(class_gaps), 'selector_gaps': len(selector_gaps), 'callback_selectors': len(callbacks),
                   'selector_gaps_unknown_owner': sum(1 for g in selector_gaps if g['owner'] == 'unknown'),
                   'c_import_gaps': len(c_gaps), 'nib_gaps': len(nib_gaps),
                   'nib_classes_missing': sum(1 for g in nib_gaps if g['status'] == 'class-missing'),
                   'nib_classes_without_decoder': sum(1 for g in nib_gaps if g['status'] == 'no-initWithCoder'),
                   'nib_classes_rejected': sum(1 for g in nib_gaps if g['status'] == 'rejected-by-nib-loader'),
                   'dynamic': len(dynamic)},
        'missing_classes': class_gaps,
        'selector_gaps': selector_gaps,
        'callback_selectors': callbacks,
        'c_import_gaps': c_gaps,
        'nib_gaps': nib_gaps,
        'nib_runtime_attributes': sorted(runtime_attrs),
        'nib_bindings': sorted(bindings),
        'dynamic': dynamic,
        'missing_class_set': sorted(missing_classes),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('bundle')
    ap.add_argument('--dump', help='ALCHEMY_DUMP_OBJC class dump (JSON)')
    ap.add_argument('--out', required=True)
    ap.add_argument('--alchemy')
    ap.add_argument('--runtime-dir', action='append', default=[])
    ap.add_argument('--root')
    ap.add_argument('--doctor-bundle', help='bundle path as doctor should see it (default: BUNDLE)')
    ap.add_argument('--attribution', default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                          'alchemy_objc_attribution.json'))
    ap.add_argument('--sources', help='Alchemy source tree (nib key coverage approximation)')
    ap.add_argument('--probe', action='append', default=[], help='stderr log of an ALCHEMY_PROBE_MISSING=zero run')
    ap.add_argument('--scan-only', action='store_true')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    scan = scan_bundle(a.bundle)
    with open(os.path.join(a.out, 'scan.json'), 'w', encoding='utf-8') as f:
        json.dump(scan, f, indent=1)
    if a.scan_only or not a.dump:
        return
    placeholders = set()
    for path in a.probe:
        with open(path, encoding='utf-8', errors='replace') as f:
            placeholders.update(re.findall(r'\[alchemy probe\] class \S+ (\S+)', f.read()))
    host = Host(a.dump, placeholders)
    doctor = doctor_imports(a.alchemy, a.runtime_dir, a.root, a.doctor_bundle or a.bundle) if a.alchemy else None
    if doctor is not None:
        with open(os.path.join(a.out, 'doctor.json'), 'w', encoding='utf-8') as f:
            json.dump(doctor, f, indent=1)
    gaps = build_gaps(scan, host, doctor, load_attribution(a.attribution), source_strings(a.sources),
                      parse_probe(a.probe), nib_loader_names(a.sources))
    with open(os.path.join(a.out, 'gaps.json'), 'w', encoding='utf-8') as f:
        json.dump(gaps, f, indent=1)
    print(json.dumps(gaps['totals'], indent=1))


if __name__ == '__main__':
    main()
