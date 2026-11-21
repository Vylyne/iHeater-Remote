#!/usr/bin/env python3
"""Config helper for install.sh and uninstall.sh.

Standard library only; runs on Python 3.7 or later.
"""
import argparse
import re
import sys
from typing import Dict, List

_VERSION_RE = re.compile(r"^#\s*CONFIG_VERSION:\s*(\d+)\s*$", re.MULTILINE)
_HEADING_RE = re.compile(r"^## (\d+)\s*$")


def _read(path):
    """Read a file without translating line endings."""
    with open(path, encoding="utf-8", newline="") as f:
        return f.read()


def _write(path, text):
    """Write in place, so symlinks and permissions survive."""
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def _eol(text):
    """Return the line ending the text already uses."""
    return "\r\n" if "\r\n" in text else "\n"


def config_version(text):
    """Return the CONFIG_VERSION number in the text, or 0 if absent."""
    m = _VERSION_RE.search(text)
    return int(m.group(1)) if m else 0


def parse_changes(text):
    # type: (str) -> Dict[int, List[str]]
    """Map each version in a changelog to its bullet lines."""
    result = {}  # type: Dict[int, List[str]]
    current = None
    in_bullet = False
    for line in text.splitlines():
        m = _HEADING_RE.match(line)
        if m:
            current = int(m.group(1))
            result[current] = []
            in_bullet = False
        elif current is None:
            continue
        elif line.startswith("- "):
            result[current].append(line)
            in_bullet = True
        elif in_bullet and line.strip() and line[0].isspace():
            result[current].append(line)
        else:
            in_bullet = False
    return result


def format_changes(text, frm, to):
    """Format the entries for versions frm+1 through to, oldest first."""
    changes = parse_changes(text)
    out = []
    for version in range(frm + 1, to + 1):
        if version in changes:
            out.append("version %d\n" % version)
            out.extend(line + "\n" for line in changes[version])
    return "".join(out)


def _cmd_version(args):
    print(config_version(_read(args.file)))
    return 0


def _cmd_changes(args):
    sys.stdout.write(format_changes(_read(args.changes_md), args.frm, args.to))
    return 0


def _build_parser():
    parser = argparse.ArgumentParser(prog="iheater_config.py")
    sub = parser.add_subparsers(dest="command")
    sub.required = True

    p = sub.add_parser("version", help="print the CONFIG_VERSION of FILE")
    p.add_argument("file")
    p.set_defaults(func=_cmd_version)

    p = sub.add_parser("changes", help="print changelog entries after FROM")
    p.add_argument("changes_md")
    p.add_argument("frm", metavar="FROM", type=int)
    p.add_argument("to", metavar="TO", type=int)
    p.set_defaults(func=_cmd_changes)

    return parser


def main(argv):
    args = _build_parser().parse_args(argv)
    try:
        return args.func(args)
    except (ValueError, OSError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
