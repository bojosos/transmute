"""Run the Swift project's gyb template expander from a pinned source checkout.

Usage: python -I alchemy_swift_gyb.py <swift-source>/utils <gyb arguments...>

The script is run with -I (isolated mode), so it puts only the checkout's
utils directory on sys.path; gyb templates import helper modules from there.
"""
import runpy
import sys


def main() -> None:
    if len(sys.argv) < 3:
        raise SystemExit("usage: alchemy_swift_gyb.py <utils-directory> <gyb arguments>")
    utils = sys.argv[1]
    sys.path.insert(0, utils)
    sys.argv = [utils + "/gyb.py"] + sys.argv[2:]
    runpy.run_path(utils + "/gyb.py", run_name="__main__")


if __name__ == "__main__":
    main()
