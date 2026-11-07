"""Summarizes `alchemy-fx doctor --deps --scan-bundle --json` output: image errors, missing
libraries and unresolved strong imports, grouped by library, with the images that need them.

Usage: python -I doctor_summary.py <doctor.json> [--symbols N] [--json out.json]
"""
import argparse
import collections
import json


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('doctor')
    parser.add_argument('--symbols', type=int, default=12, help='symbols listed per library')
    parser.add_argument('--json', help='also write the summary as JSON')
    args = parser.parse_args()
    with open(args.doctor, encoding='utf-8') as f:
        report = json.load(f)
    root = report.get('root', '')
    bundle = root.split('\\Contents\\')[0] if '\\Contents\\' in root else root

    def short(path):
        return path[len(bundle):].lstrip('\\') if path.startswith(bundle) else path

    statuses = collections.Counter()
    missing_libraries = collections.defaultdict(set)
    missing_symbols = collections.defaultdict(lambda: collections.defaultdict(set))
    errors = []
    for image in report['images']:
        name = short(image['path'])
        if image.get('error'):
            errors.append((name, image['error']))
        for dependency in image.get('dependencies', []):
            statuses['dep:' + dependency['status']] += 1
            if dependency['status'] not in ('guest', 'provider') and not dependency.get('weak'):
                missing_libraries[dependency['identity']].add(name)
        for imported in image.get('imports', []):
            statuses['import:' + imported['status']] += 1
            if imported['status'] not in ('guest', 'provider') and not imported.get('weak'):
                missing_symbols[imported['library']][imported['symbol']].add(name)

    print(f"images {len(report['images'])}; statuses {dict(statuses)}")
    print(f'image errors: {len(errors)}')
    for name, error in errors[:20]:
        print(f'  {name}: {error[:200]}')
    print(f'missing libraries: {len(missing_libraries)}')
    for identity, users in sorted(missing_libraries.items()):
        print(f'  {identity}  <- {len(users)} image(s), e.g. {sorted(users)[0]}')
    total = sum(len(symbols) for symbols in missing_symbols.values())
    print(f'unresolved strong imports: {total} symbols in {len(missing_symbols)} libraries')
    for library, symbols in sorted(missing_symbols.items(), key=lambda item: -len(item[1])):
        print(f'  {library}: {len(symbols)}')
        for symbol, users in sorted(symbols.items())[:args.symbols]:
            print(f'    {symbol}  <- {len(users)}: {", ".join(sorted(users)[:2])}')
    if args.json:
        with open(args.json, 'w', encoding='utf-8') as f:
            json.dump({
                'errors': errors,
                'missing_libraries': {k: sorted(v) for k, v in missing_libraries.items()},
                'missing_symbols': {lib: {s: sorted(u) for s, u in syms.items()}
                                    for lib, syms in missing_symbols.items()},
            }, f, indent=1)


if __name__ == '__main__':
    main()
