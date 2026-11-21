"""Unit tests for iheater_config.py.

Run: python -m unittest discover -s scripts -p "test_*.py" -v
"""
import contextlib
import io
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import iheater_config  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHIPPED_CFG = os.path.join(REPO, "klipper", "iheater_remote.cfg")
SHIPPED_CHANGES = os.path.join(REPO, "klipper", "CONFIG_CHANGES.md")

CHANGES_MD = """# Config changes

## 3
- Rename `foo` to `bar`.
  Edit your copy.

## 2
- Drop `baz`.

## 1
- First versioned config. No changes to make.
"""


class VersionTests(unittest.TestCase):
    def test_version_present(self):
        self.assertEqual(
            iheater_config.config_version("# t\n# CONFIG_VERSION: 3\n[x]\n"), 3)

    def test_version_absent_is_zero(self):
        self.assertEqual(
            iheater_config.config_version("[heater_virtual chamber]\n"), 0)

    def test_version_malformed_is_zero(self):
        self.assertEqual(
            iheater_config.config_version("# CONFIG_VERSION: abc\n"), 0)

    def test_version_crlf(self):
        self.assertEqual(
            iheater_config.config_version("# CONFIG_VERSION: 2\r\n"), 2)


class ChangesTests(unittest.TestCase):
    def test_parse_changes(self):
        got = iheater_config.parse_changes(CHANGES_MD)
        self.assertEqual(got, {
            3: ["- Rename `foo` to `bar`.", "  Edit your copy."],
            2: ["- Drop `baz`."],
            1: ["- First versioned config. No changes to make."],
        })

    def test_format_between(self):
        self.assertEqual(
            iheater_config.format_changes(CHANGES_MD, 1, 3),
            "version 2\n- Drop `baz`.\n"
            "version 3\n- Rename `foo` to `bar`.\n  Edit your copy.\n")

    def test_format_from_zero_includes_1(self):
        out = iheater_config.format_changes(CHANGES_MD, 0, 1)
        self.assertTrue(out.startswith("version 1\n"))

    def test_format_nothing_when_not_older(self):
        self.assertEqual(iheater_config.format_changes(CHANGES_MD, 3, 3), "")
        self.assertEqual(iheater_config.format_changes(CHANGES_MD, 4, 3), "")

    def test_format_missing_version_prints_nothing(self):
        md = CHANGES_MD.replace("## 2\n- Drop `baz`.\n\n", "")
        out = iheater_config.format_changes(md, 1, 3)
        self.assertNotIn("version 2", out)
        self.assertTrue(out.startswith("version 3\n"))

    def test_text_outside_bullets_ignored(self):
        md = CHANGES_MD.replace("## 2\n", "## 2\nA stray paragraph.\n")
        out = iheater_config.format_changes(md, 1, 3)
        self.assertNotIn("stray", out)
        self.assertIn("- Drop `baz`.", out)

    def test_cli_version(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "x.cfg")
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write("# CONFIG_VERSION: 1\n")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = iheater_config.main(["version", path])
        self.assertEqual(rc, 0)
        self.assertEqual(out.getvalue(), "1\n")

    def test_cli_missing_file_exits_1(self):
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            rc = iheater_config.main(["version", "/no/such"])
        self.assertEqual(rc, 1)
        self.assertTrue(err.getvalue())

    def test_shipped_cfg_has_version_1(self):
        self.assertEqual(
            iheater_config.config_version(iheater_config._read(SHIPPED_CFG)), 1)

    def test_shipped_changes_cover_shipped_version(self):
        version = iheater_config.config_version(
            iheater_config._read(SHIPPED_CFG))
        changes = iheater_config.parse_changes(
            iheater_config._read(SHIPPED_CHANGES))
        self.assertIn(version, changes)


if __name__ == "__main__":
    unittest.main()
