#!/usr/bin/env python3
"""Config helper for install.sh and uninstall.sh.

Standard library only; runs on Python 3.7 or later.
"""
import argparse
import re
import sys
from typing import Dict, List, Optional, Tuple

_VERSION_RE = re.compile(r"^#\s*CONFIG_VERSION:\s*(\d+)\s*$", re.MULTILINE)
_HEADING_RE = re.compile(r"^## (\d+)\s*$")
_TOKEN_RE = re.compile(r"^[0-9A-HJKMNP-TV-Z]{8}$")
_SECTION_RE = re.compile(r"^\[heater_virtual\s+([^\]\s]+)\s*\]")
_CARRIED_KEYS = ("remote_token", "sensor", "max_target")


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


def check_token(token):
    """Return True if the token is a valid Remote token."""
    return bool(_TOKEN_RE.match(token.replace("-", "").upper()))


def _find_section(lines):
    # type: (List[str]) -> Optional[Tuple[int, int, str]]
    """Locate the first [heater_virtual ...] section.

    Returns (header index, end index, name); the section runs to the next
    line starting with "[".
    """
    for i, line in enumerate(lines):
        m = _SECTION_RE.match(line)
        if m:
            end = len(lines)
            for j in range(i + 1, len(lines)):
                if lines[j].startswith("["):
                    end = j
                    break
            return i, end, m.group(1)
    return None


def _split_value(line):
    """Return the stripped text after the first ':' or '='."""
    return re.split(r"[:=]", line, maxsplit=1)[1].strip()


def _key_res(key):
    k = re.escape(key)
    return (re.compile(r"^%s\s*[:=]" % k, re.IGNORECASE),
            re.compile(r"^#\s*%s\s*[:=]" % k, re.IGNORECASE))


def get_key(text, key):
    # type: (str, str) -> Optional[str]
    """Return the active value of key in the first heater_virtual section."""
    lines = text.splitlines(True)
    sec = _find_section(lines)
    if sec is None:
        return None
    active, _ = _key_res(key)
    for line in lines[sec[0] + 1:sec[1]]:
        if active.match(line):
            return _split_value(line)
    return None


def set_key(text, key, value):
    # type: (str, str, str) -> str
    """Set key in the first heater_virtual section.

    An active line is replaced, else a commented one is uncommented, else
    the line is inserted after the section header.
    """
    lines = text.splitlines(True)
    sec = _find_section(lines)
    if sec is None:
        raise ValueError("no [heater_virtual ...] section found")
    start, end, _ = sec
    eol = _eol(text)
    new_line = "%s: %s" % (key, value.strip())
    active, commented = _key_res(key)
    for pattern in (active, commented):
        for i in range(start + 1, end):
            if pattern.match(lines[i]):
                body = lines[i].rstrip("\r\n")
                lines[i] = new_line + lines[i][len(body):]
                return "".join(lines)
    if not lines[start].endswith("\n"):
        lines[start] += eol
    lines.insert(start + 1, new_line + eol)
    return "".join(lines)


def carry(old, new):
    # type: (str, str) -> Tuple[str, List[str], Optional[str]]
    """Copy the user's settings from old into new.

    Returns the new text, the keys carried and old's section name when it
    is not "chamber".
    """
    carried = []  # type: List[str]
    for key in _CARRIED_KEYS:
        value = get_key(old, key)
        if value is not None:
            new = set_key(new, key, value)
            carried.append(key)
    sec = _find_section(old.splitlines(True))
    renamed = sec[2] if sec and sec[2] != "chamber" else None
    return new, carried, renamed


def _cmd_version(args):
    print(config_version(_read(args.file)))
    return 0


def _cmd_changes(args):
    sys.stdout.write(format_changes(_read(args.changes_md), args.frm, args.to))
    return 0


def _cmd_check_token(args):
    return 0 if check_token(args.token) else 1


def _cmd_get_key(args):
    text = _read(args.file)
    if _find_section(text.splitlines(True)) is None:
        raise ValueError("no [heater_virtual ...] section in %s" % args.file)
    value = get_key(text, args.key)
    if value is None:
        return 1
    print(value)
    return 0


def _cmd_set_key(args):
    _write(args.file, set_key(_read(args.file), args.key, args.value))
    return 0


def _cmd_carry(args):
    text, carried, renamed = carry(_read(args.old), _read(args.new))
    _write(args.new, text)
    for key in carried:
        print("carried %s" % key)
    if renamed:
        print("renamed %s" % renamed)
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

    p = sub.add_parser("check-token", help="exit 0 if TOKEN is valid")
    p.add_argument("token")
    p.set_defaults(func=_cmd_check_token)

    p = sub.add_parser("get-key", help="print the active value of KEY")
    p.add_argument("file")
    p.add_argument("key")
    p.set_defaults(func=_cmd_get_key)

    p = sub.add_parser("set-key", help="set KEY to VALUE in FILE")
    p.add_argument("file")
    p.add_argument("key")
    p.add_argument("value")
    p.set_defaults(func=_cmd_set_key)

    p = sub.add_parser("carry", help="carry settings from OLD into NEW")
    p.add_argument("old")
    p.add_argument("new")
    p.set_defaults(func=_cmd_carry)

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
