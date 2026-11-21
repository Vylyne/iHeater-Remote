#!/usr/bin/env python3
"""Config helper for install.sh and uninstall.sh.

Standard library only; runs on Python 3.7 or later.
"""
import argparse
import fnmatch
import json
import posixpath
import re
import sys
from typing import Dict, List, Optional, Tuple

_VERSION_RE = re.compile(r"^#\s*CONFIG_VERSION:\s*(\d+)\s*$", re.MULTILINE)
_HEADING_RE = re.compile(r"^## (\d+)\s*$")
_TOKEN_RE = re.compile(r"^[0-9A-HJKMNP-TV-Z]{8}$")
_SECTION_RE = re.compile(r"^\[heater_virtual\s+([^\]\s]+)\s*\]")
_INCLUDE_RE = re.compile(
    r"^\[include\s+(.+?)\s*\](\s*[#;].*)?$", re.IGNORECASE)
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


def _include_specs(lines):
    # type: (List[str]) -> List[Tuple[int, str]]
    """Return (line index, spec) for every active include line."""
    found = []
    for i, line in enumerate(lines):
        m = _INCLUDE_RE.match(line.rstrip("\r\n"))
        if m:
            found.append((i, m.group(1).strip()))
    return found


def _split_path(config_dir, spec):
    # type: (str, str) -> List[str]
    return posixpath.normpath(posixpath.join(config_dir, spec)).split("/")


def include_kind(printer_text, config_dir, target):
    # type: (str, str, str) -> Optional[str]
    """Return "exact" or "glob" if an include already covers target."""
    want = _split_path(config_dir, target)
    kind = None
    for _, spec in _include_specs(printer_text.splitlines(True)):
        got = _split_path(config_dir, spec)
        if len(got) == len(want) and all(
                fnmatch.fnmatchcase(w, g) for g, w in zip(got, want)):
            if spec == target:
                return "exact"
            kind = "glob"
    return kind


def add_include(printer_text, target):
    # type: (str, str) -> str
    """Insert an include line after the last include, or at line 1."""
    lines = printer_text.splitlines(True)
    eol = _eol(printer_text)
    specs = _include_specs(lines)
    at = specs[-1][0] + 1 if specs else 0
    if at > 0 and not lines[at - 1].endswith(("\n", "\r")):
        lines[at - 1] += eol
    lines.insert(at, "[include %s]%s" % (target, eol))
    return "".join(lines)


def remove_include(printer_text, target):
    # type: (str, str) -> str
    """Remove every active include line whose spec equals target."""
    lines = printer_text.splitlines(True)
    drop = set(i for i, spec in _include_specs(lines) if spec == target)
    return "".join(l for i, l in enumerate(lines) if i not in drop)


def _is_header(line, name):
    # type: (str, str) -> bool
    stripped = line.strip()
    if not (stripped.startswith("[") and stripped.endswith("]")):
        return False
    return re.sub(r"\s+", " ", stripped[1:-1].strip()) == name


def has_section(text, name):
    # type: (str, str) -> bool
    return any(_is_header(l, name) for l in text.splitlines(True))


def _is_comment(line):
    return line.lstrip()[:1] in ("#", ";") and bool(line.strip())


def _kept_after_section(body):
    # type: (List[str]) -> List[str]
    """Return the lines at the end of a removed body that stay behind.

    A comment run directly above the next header, preceded by a blank line,
    belongs to the next section. Otherwise only trailing blank lines stay.
    """
    end = len(body)
    c = end
    while c > 0 and _is_comment(body[c - 1]):
        c -= 1
    if c < end and c > 0 and not body[c - 1].strip():
        b = c - 1
        while b > 0 and not body[b - 1].strip():
            b -= 1
        return body[b:]
    if c < end:
        return []
    b = end
    while b > 0 and not body[b - 1].strip():
        b -= 1
    return body[b:]


def remove_section(text, name):
    # type: (str, str) -> str
    """Remove every [name] section and one blank line above it.

    When another section follows, the blank lines (and any comment block)
    that belong to it stay, so the neighbours remain separated.
    """
    lines = text.splitlines(True)
    out = []  # type: List[str]
    i = 0
    while i < len(lines):
        if not _is_header(lines[i], name):
            out.append(lines[i])
            i += 1
            continue
        if out and not out[-1].strip():
            out.pop()
        j = i + 1
        while j < len(lines) and not lines[j].startswith("["):
            j += 1
        if j < len(lines):
            out.extend(_kept_after_section(lines[i + 1:j]))
        i = j
    return "".join(out)


def append_block(text, block):
    # type: (str, str) -> str
    """Append block after one blank line, using the text's line endings."""
    eol = _eol(text)
    if text and not text.endswith(("\n", "\r")):
        text += eol
    block = block.replace("\r\n", "\n").replace("\n", eol)
    if block and not block.endswith(eol):
        block += eol
    return text + eol + block


def print_state(json_text):
    # type: (str) -> str
    """Return print_stats.state from a Moonraker objects/query reply."""
    try:
        reply = json.loads(json_text)
        state = reply["result"]["status"]["print_stats"]["state"]
    except (KeyError, TypeError, ValueError) as e:
        raise ValueError("no print state in reply (%s)" % e)
    if not isinstance(state, str):
        raise ValueError("print state is not a string: %r" % (state,))
    return state


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


def _cmd_has_include(args):
    kind = include_kind(_read(args.printer_cfg), args.config_dir, args.target)
    if kind is None:
        return 1
    print(kind)
    return 0


def _cmd_add_include(args):
    _write(args.printer_cfg, add_include(_read(args.printer_cfg), args.target))
    return 0


def _cmd_remove_include(args):
    _write(args.printer_cfg,
           remove_include(_read(args.printer_cfg), args.target))
    return 0


def _cmd_has_section(args):
    return 0 if has_section(_read(args.conf), args.name) else 1


def _cmd_remove_section(args):
    _write(args.conf, remove_section(_read(args.conf), args.name))
    return 0


def _cmd_append_block(args):
    block = _read(args.template)
    for pair in args.pairs:
        name, sep, value = pair.partition("=")
        if not sep or not name:
            raise ValueError("expected NAME=VALUE, got %r" % pair)
        block = block.replace("<%s>" % name, value)
    _write(args.conf, append_block(_read(args.conf), block))
    return 0


def _cmd_print_state(args):
    print(print_state(sys.stdin.read()))
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

    p = sub.add_parser("has-include", 
                       help="exit 0 if an include covers TARGET")
    p.add_argument("printer_cfg")
    p.add_argument("config_dir")
    p.add_argument("target")
    p.set_defaults(func=_cmd_has_include)

    p = sub.add_parser("add-include", help="add an include of TARGET")
    p.add_argument("printer_cfg")
    p.add_argument("target")
    p.set_defaults(func=_cmd_add_include)

    p = sub.add_parser("remove-include", help="remove the include of TARGET")
    p.add_argument("printer_cfg")
    p.add_argument("target")
    p.set_defaults(func=_cmd_remove_include)

    p = sub.add_parser("has-section", help="exit 0 if [NAME] is in CONF")
    p.add_argument("conf")
    p.add_argument("name")
    p.set_defaults(func=_cmd_has_section)

    p = sub.add_parser("remove-section", help="remove [NAME] from CONF")
    p.add_argument("conf")
    p.add_argument("name")
    p.set_defaults(func=_cmd_remove_section)

    p = sub.add_parser("append-block", help="append TEMPLATE to CONF")
    p.add_argument("conf")
    p.add_argument("template")
    p.add_argument("pairs", nargs="*", metavar="NAME=VALUE")
    p.set_defaults(func=_cmd_append_block)

    p = sub.add_parser("print-state", help="read print state JSON on stdin")
    p.set_defaults(func=_cmd_print_state)

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
