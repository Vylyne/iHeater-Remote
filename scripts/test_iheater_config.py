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


FIXTURE = (
    "[heater_virtual chamber]\n"
    "sensor: temperature_sensor chamber_hexa\n"
    "#max_target: 80\n"
    "# Copy from the Remote's Setup page.\n"
    "#remote_token: XXXX-XXXX\n"
    "\n"
    "[gcode_macro M141]\n"
)


class TokenTests(unittest.TestCase):
    def test_token_valid(self):
        self.assertTrue(iheater_config.check_token("abcd-efgh"))
        self.assertTrue(iheater_config.check_token("ABCDEFGH"))

    def test_token_invalid(self):
        for bad in ("ABCD-EFGI", "ABCDEFG", "", "ABCD EFGH"):
            self.assertFalse(iheater_config.check_token(bad), bad)

    def test_cli_check_token(self):
        self.assertEqual(iheater_config.main(["check-token", "ABCD-EFGH"]), 0)
        self.assertEqual(iheater_config.main(["check-token", "ABCD-EFGI"]), 1)


class KeyTests(unittest.TestCase):
    def lines(self, text):
        return text.split("\n")

    def test_set_key_replaces_active(self):
        out = iheater_config.set_key(
            FIXTURE, "sensor", "temperature_sensor x")
        want = self.lines(FIXTURE)
        want[1] = "sensor: temperature_sensor x"
        self.assertEqual(self.lines(out), want)

    def test_set_key_uncomments(self):
        out = iheater_config.set_key(FIXTURE, "remote_token", "ABCD-EFGH")
        want = self.lines(FIXTURE)
        want[4] = "remote_token: ABCD-EFGH"
        self.assertEqual(self.lines(out), want)

    def test_set_key_inserts_after_header(self):
        out = iheater_config.set_key(FIXTURE, "foo", "1")
        want = self.lines(FIXTURE)
        want.insert(1, "foo: 1")
        self.assertEqual(self.lines(out), want)

    def test_set_key_active_wins_over_comment(self):
        text = "[heater_virtual chamber]\n#sensor: a\nsensor: b\n"
        out = iheater_config.set_key(text, "sensor", "c")
        self.assertEqual(out, "[heater_virtual chamber]\n#sensor: a\nsensor: c\n")

    def test_set_key_ignores_other_sections(self):
        text = (FIXTURE + "[heater_virtual other]\nsensor: keep\n")
        out = iheater_config.set_key(text, "sensor", "new")
        self.assertTrue(out.endswith("[heater_virtual other]\nsensor: keep\n"))

    def test_set_key_no_section_raises(self):
        with self.assertRaises(ValueError):
            iheater_config.set_key("[gcode_macro M141]\n", "sensor", "x")

    def test_set_key_keeps_crlf(self):
        crlf = FIXTURE.replace("\n", "\r\n")
        for key in ("sensor", "remote_token", "foo"):
            out = iheater_config.set_key(crlf, key, "v")
            self.assertEqual(out.replace("\r\n", ""), out.replace("\r", "")
                             .replace("\n", ""))
            self.assertNotIn("\n", out.replace("\r\n", ""))
            self.assertNotIn("\r", out.replace("\r\n", ""))

    def test_set_key_strips_value(self):
        out = iheater_config.set_key(FIXTURE, "foo", "  1 \n")
        self.assertIn("\nfoo: 1\n", out)

    def test_indented_key_is_not_active(self):
        text = "[heater_virtual chamber]\nsensor: a\n  sensor: x\n"
        self.assertEqual(iheater_config.get_key(text, "sensor"), "a")
        out = iheater_config.set_key(text, "sensor", "b")
        self.assertEqual(out, "[heater_virtual chamber]\nsensor: b\n  sensor: x\n")
        only = "[heater_virtual chamber]\n  sensor: x\n"
        self.assertIsNone(iheater_config.get_key(only, "sensor"))

    def test_get_key(self):
        self.assertEqual(iheater_config.get_key(FIXTURE, "sensor"),
                         "temperature_sensor chamber_hexa")
        self.assertIsNone(iheater_config.get_key(FIXTURE, "remote_token"))

    def test_get_key_no_section(self):
        self.assertIsNone(iheater_config.get_key("[x]\nsensor: a\n", "sensor"))

    def _run(self, argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = iheater_config.main(argv)
        return rc, out.getvalue(), err.getvalue()

    def test_cli_get_and_set_key(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "a.cfg")
            iheater_config._write(path, FIXTURE)
            rc, out, err = self._run(["get-key", path, "sensor"])
            self.assertEqual((rc, out, err), (
                0, "temperature_sensor chamber_hexa\n", ""))
            rc, out, err = self._run(["get-key", path, "remote_token"])
            self.assertEqual((rc, out, err), (1, "", ""))
            self.assertEqual(self._run(
                ["set-key", path, "remote_token", "ABCD-EFGH"])[0], 0)
            rc, out, err = self._run(["get-key", path, "remote_token"])
            self.assertEqual((rc, out), (0, "ABCD-EFGH\n"))
            rc, out, err = self._run(["get-key", os.path.join(d, "no"), "x"])
            self.assertEqual(rc, 1)
            self.assertTrue(err)

    def test_cli_get_key_no_section_is_error(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "a.cfg")
            iheater_config._write(path, "[x]\n")
            rc, out, err = self._run(["get-key", path, "sensor"])
            self.assertEqual((rc, out), (1, ""))
            self.assertTrue(err)


OLD_FULL = (
    "[heater_virtual chamber]\n"
    "remote_token: abcd-efgh  # mine\n"
    "sensor: temperature_sensor other\n"
    "max_target: 70\n"
)


class CarryTests(unittest.TestCase):
    def test_carry_values(self):
        out, carried, renamed = iheater_config.carry(OLD_FULL, FIXTURE)
        self.assertIn("remote_token: abcd-efgh  # mine\n", out)
        self.assertIn("sensor: temperature_sensor other\n", out)
        self.assertIn("max_target: 70\n", out)
        self.assertEqual(carried, ["remote_token", "sensor", "max_target"])
        self.assertIsNone(renamed)

    def test_carry_skips_commented(self):
        old = "[heater_virtual chamber]\n#max_target: 60\n"
        out, carried, renamed = iheater_config.carry(old, FIXTURE)
        self.assertIn("#max_target: 80\n", out)
        self.assertNotIn("max_target", carried)

    def test_carry_reports_rename(self):
        old = OLD_FULL.replace("chamber", "hexa")
        out, carried, renamed = iheater_config.carry(old, FIXTURE)
        self.assertEqual(renamed, "hexa")
        self.assertIn("[heater_virtual chamber]\n", out)

    def test_carry_cli_output(self):
        with tempfile.TemporaryDirectory() as d:
            old = os.path.join(d, "old.cfg")
            new = os.path.join(d, "new.cfg")
            iheater_config._write(
                old, "[heater_virtual hexa]\nremote_token: ABCD-EFGH\n")
            iheater_config._write(new, FIXTURE)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = iheater_config.main(["carry", old, new])
            self.assertEqual(rc, 0)
            self.assertEqual(out.getvalue(), "carried remote_token\nrenamed hexa\n")
            self.assertIn("remote_token: ABCD-EFGH\n", iheater_config._read(new))



CONFIG_DIR = "/home/pi/printer_data/config"
TARGET = "addons/iheater_remote.cfg"
INCLUDE_LINE = "[include addons/iheater_remote.cfg]"
SAVE_CONFIG = (
    "[printer]\nkinematics: corexy\n\n"
    "#*# <---------------------- SAVE_CONFIG ---------------------->\n"
    "#*# [probe]\n")
ROUND_TRIP_FIXTURES = [
    "[include a.cfg]\n[include b.cfg]\n\n[printer]\n",
    "[printer]\nkinematics: corexy\n",
    SAVE_CONFIG,
    "[include a.cfg]\r\n\r\n[printer]\r\n",
    "",
]


class IncludeTests(unittest.TestCase):
    def _kind(self, text):
        return iheater_config.include_kind(text, CONFIG_DIR, TARGET)

    def _cli(self, argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = iheater_config.main(argv)
        return rc, out.getvalue(), err.getvalue()

    def test_has_include_exact(self):
        self.assertEqual(self._kind(INCLUDE_LINE + "\n"), "exact")

    def test_has_include_glob(self):
        self.assertEqual(self._kind("[include addons/*.cfg]\n"), "glob")

    def test_glob_does_not_cross_dirs(self):
        self.assertIsNone(self._kind("[include *.cfg]\n"))

    def test_has_include_dot_slash(self):
        self.assertEqual(
            self._kind("[include ./addons/iheater_remote.cfg]\n"), "glob")

    def test_has_include_absolute(self):
        self.assertEqual(self._kind(
            "[include /home/pi/printer_data/config/addons/"
            "iheater_remote.cfg]\n"), "glob")

    def test_commented_include_ignored(self):
        self.assertIsNone(self._kind("#" + INCLUDE_LINE + "\n"))

    def test_include_with_trailing_comment(self):
        self.assertEqual(self._kind(INCLUDE_LINE + "  # chamber\n"), "exact")

    def test_add_after_last_include(self):
        got = iheater_config.add_include(
            "[include a.cfg]\n[include b.cfg]\n\n[printer]\n", TARGET)
        self.assertEqual(
            got, "[include a.cfg]\n[include b.cfg]\n" + INCLUDE_LINE
            + "\n\n[printer]\n")

    def test_add_at_top_when_none(self):
        got = iheater_config.add_include(
            "[printer]\nkinematics: corexy\n", TARGET)
        self.assertEqual(
            got, INCLUDE_LINE + "\n[printer]\nkinematics: corexy\n")

    def test_add_above_save_config(self):
        got = iheater_config.add_include(SAVE_CONFIG, TARGET)
        self.assertEqual(got, INCLUDE_LINE + "\n" + SAVE_CONFIG)

    def test_add_last_line_no_newline(self):
        got = iheater_config.add_include("[include a.cfg]", TARGET)
        self.assertEqual(got, "[include a.cfg]\n" + INCLUDE_LINE + "\n")

    def test_add_keeps_crlf(self):
        got = iheater_config.add_include(
            "[include a.cfg]\r\n[printer]\r\n", TARGET)
        self.assertEqual(
            got, "[include a.cfg]\r\n" + INCLUDE_LINE + "\r\n[printer]\r\n")

    def test_add_remove_round_trip(self):
        for t in ROUND_TRIP_FIXTURES:
            with self.subTest(t=t):
                added = iheater_config.add_include(t, TARGET)
                self.assertEqual(
                    iheater_config.remove_include(added, TARGET), t)

    def test_remove_ignores_other_and_commented(self):
        t = "#" + INCLUDE_LINE + "\n[include b.cfg]\n"
        self.assertEqual(iheater_config.remove_include(t, TARGET), t)

    def test_remove_with_trailing_comment(self):
        t = "[include a.cfg]\n" + INCLUDE_LINE + " # x\n[printer]\n"
        self.assertEqual(
            iheater_config.remove_include(t, TARGET),
            "[include a.cfg]\n[printer]\n")

    def test_cli_has_include(self):
        with tempfile.TemporaryDirectory() as d:
            cfg = os.path.join(d, "printer.cfg")
            iheater_config._write(cfg, "[include addons/*.cfg]\n")
            self.assertEqual(
                self._cli(["has-include", cfg, CONFIG_DIR, TARGET]),
                (0, "glob\n", ""))
            iheater_config._write(cfg, "[printer]\n")
            self.assertEqual(
                self._cli(["has-include", cfg, CONFIG_DIR, TARGET]),
                (1, "", ""))

    def test_cli_add_remove_include(self):
        with tempfile.TemporaryDirectory() as d:
            cfg = os.path.join(d, "printer.cfg")
            iheater_config._write(cfg, "[printer]\n")
            self.assertEqual(
                iheater_config.main(["add-include", cfg, TARGET]), 0)
            self.assertEqual(iheater_config._read(cfg),
                             INCLUDE_LINE + "\n[printer]\n")
            self.assertEqual(
                iheater_config.main(["remove-include", cfg, TARGET]), 0)
            self.assertEqual(iheater_config._read(cfg), "[printer]\n")

    def test_cli_missing_file_is_error(self):
        rc, _, err = self._cli(
            ["has-include", "/nonexistent/printer.cfg", CONFIG_DIR, TARGET])
        self.assertEqual(rc, 1)
        self.assertIn("error", err)


MOONRAKER_CONF = (
    "[server]\nhost: 0.0.0.0\n\n"
    "[iheater_remote]\nfoo: bar\n\n"
    "[authorization]\ncors_domains:\n")


class SectionTests(unittest.TestCase):
    def test_has_section(self):
        self.assertTrue(
            iheater_config.has_section(MOONRAKER_CONF, "iheater_remote"))
        self.assertFalse(iheater_config.has_section(
            "[iheater_remote_x]\n", "iheater_remote"))
        self.assertTrue(iheater_config.has_section(
            "[update_manager  iheater-remote]\n",
            "update_manager iheater-remote"))

    def test_remove_section_middle(self):
        self.assertEqual(
            iheater_config.remove_section(MOONRAKER_CONF, "iheater_remote"),
            "[server]\nhost: 0.0.0.0\n\n[authorization]\ncors_domains:\n")

    def test_remove_section_last(self):
        t = "[server]\nhost: 0.0.0.0\n\n[iheater_remote]\nfoo: bar\n"
        self.assertEqual(
            iheater_config.remove_section(t, "iheater_remote"),
            "[server]\nhost: 0.0.0.0\n")

    def test_remove_section_absent_unchanged(self):
        self.assertEqual(
            iheater_config.remove_section(MOONRAKER_CONF, "nope"),
            MOONRAKER_CONF)

    def test_append_remove_round_trip(self):
        block = "[iheater_remote]\nfoo: bar\n"
        for t in ("[server]\nhost: 0.0.0.0\n",
                  "[server]\r\nhost: 0.0.0.0\r\n",
                  ""):
            with self.subTest(t=t):
                added = iheater_config.append_block(t, block)
                self.assertTrue(
                    iheater_config.has_section(added, "iheater_remote"))
                self.assertEqual(
                    iheater_config.remove_section(added, "iheater_remote"), t)

    def test_append_converts_block_to_crlf(self):
        got = iheater_config.append_block("[a]\r\nx: 1\r\n", "[b]\ny: 2\n")
        self.assertEqual(got, "[a]\r\nx: 1\r\n\r\n[b]\r\ny: 2\r\n")

    def test_cli_append_block_substitutes(self):
        with tempfile.TemporaryDirectory() as d:
            conf = os.path.join(d, "moonraker.conf")
            tpl = os.path.join(d, "block.tpl")
            iheater_config._write(conf, "[server]\nhost: 0.0.0.0\n")
            iheater_config._write(
                tpl, "[iheater_remote]\npath: <INSTALL_PATH>\nx: <OTHER>\n")
            rc = iheater_config.main(
                ["append-block", conf, tpl,
                 "INSTALL_PATH=/home/pi/iHeater-Remote", "OTHER=a&b|c=d"])
            self.assertEqual(rc, 0)
            self.assertEqual(
                iheater_config._read(conf),
                "[server]\nhost: 0.0.0.0\n\n[iheater_remote]\n"
                "path: /home/pi/iHeater-Remote\nx: a&b|c=d\n")

    def test_cli_has_and_remove_section(self):
        with tempfile.TemporaryDirectory() as d:
            conf = os.path.join(d, "moonraker.conf")
            iheater_config._write(conf, MOONRAKER_CONF)
            main = iheater_config.main
            self.assertEqual(main(["has-section", conf, "iheater_remote"]), 0)
            self.assertEqual(main(["has-section", conf, "nope"]), 1)
            self.assertEqual(
                main(["remove-section", conf, "iheater_remote"]), 0)
            self.assertEqual(main(["has-section", conf, "iheater_remote"]), 1)


GOOD_STATE = '{"result":{"status":{"print_stats":{"state":"printing"}}}}'


class PrintStateTests(unittest.TestCase):
    def test_print_state(self):
        self.assertEqual(iheater_config.print_state(GOOD_STATE), "printing")

    def test_print_state_bad(self):
        for text in ("not json", "{}", '{"result":{"status":{}}}',
                     '{"result":[]}', "null"):
            with self.subTest(text=text):
                with self.assertRaises(ValueError):
                    iheater_config.print_state(text)

    def _cli(self, text):
        out, err = io.StringIO(), io.StringIO()
        old = sys.stdin
        sys.stdin = io.StringIO(text)
        try:
            with contextlib.redirect_stdout(out), \
                    contextlib.redirect_stderr(err):
                rc = iheater_config.main(["print-state"])
        finally:
            sys.stdin = old
        return rc, out.getvalue(), err.getvalue()

    def test_cli_print_state(self):
        self.assertEqual(self._cli(GOOD_STATE), (0, "printing\n", ""))

    def test_cli_print_state_bad(self):
        for text in ("not json", "{}"):
            rc, out, err = self._cli(text)
            self.assertEqual((rc, out), (1, ""))
            self.assertIn("error", err)


if __name__ == "__main__":
    unittest.main()
