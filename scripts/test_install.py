"""Smoke tests for install.sh (and, later, uninstall.sh).

Each test builds a fake printer tree in a temporary directory, puts stub
systemctl, sudo and curl scripts first on PATH, and runs the real script with
bash. Nothing outside the temporary directory is touched.

Run: python -m unittest discover -s scripts -p "test_install.py" -v
"""
import glob
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

STATE_STANDBY = '{"result":{"status":{"print_stats":{"state":"standby"}}}}'
STATE_PRINTING = '{"result":{"status":{"print_stats":{"state":"printing"}}}}'

STUBS = {
    "systemctl": (
        "#!/bin/sh\n"
        'case "$1" in\n'
        "  list-unit-files)\n"
        '    for a in "$@"; do last="$a"; done\n'
        '    echo "$last enabled"\n'
        "    ;;\n"
        "  restart)\n"
        '    echo "restart $2" >> "$STUB_LOG"\n'
        "    ;;\n"
        "esac\n"
    ),
    "sudo": '#!/bin/sh\nexec "$@"\n',
    "curl": (
        "#!/bin/sh\n"
        'if [ -n "${STUB_PRINT_JSON+x}" ]; then\n'
        '  echo "$STUB_PRINT_JSON"\n'
        "else\n"
        "  exit 7\n"
        "fi\n"
    ),
}


def write_lf(path, text):
    """Write text with LF endings, whatever the platform."""
    with open(str(path), "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def read_text(path):
    with open(str(path), encoding="utf-8", newline="") as handle:
        return handle.read()


def bash_is_usable():
    """True when a POSIX bash exists and symlinks work in a temp directory."""
    bash = shutil.which("bash")
    if not bash:
        return False
    if "system32" in bash.lower().replace("/", "\\").split("\\"):
        return False  # WSL's bash: it cannot see the Windows paths
    try:
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "target"
            target.write_text("x")
            os.symlink(str(target), str(Path(tmp) / "link"))
    except (OSError, NotImplementedError):
        return False
    return True


class Fixture:
    """A fake printer tree plus the stub bin directory."""

    def __init__(self, root):
        self.root = Path(root)
        self.klipper = self.root / "klipper"
        self.moonraker = self.root / "moonraker"
        self.printer_data = self.root / "printer_data"
        self.config_dir = self.printer_data / "config"
        self.bin = self.root / "bin"
        self.log = self.root / "stub.log"
        self.extras = self.klipper / "klippy" / "extras"
        self.components = self.moonraker / "moonraker" / "components"
        self.addon_cfg = self.config_dir / "addons" / "iheater_remote.cfg"
        self.printer_cfg = self.config_dir / "printer.cfg"
        self.moonraker_conf = self.config_dir / "moonraker.conf"

        self.extras.mkdir(parents=True)
        self.components.mkdir(parents=True)
        self.config_dir.mkdir(parents=True)
        self.bin.mkdir()
        write_lf(self.printer_cfg,
                 "[printer]\n#*# <---- SAVE_CONFIG ---->\n"
                 "#*# DO NOT EDIT THIS BLOCK OR BELOW.\n")
        write_lf(self.moonraker_conf, "[server]\nhost: 0.0.0.0\n")
        write_lf(self.log, "")
        for name, body in STUBS.items():
            self.add_stub(name, body)

    def add_stub(self, name, body):
        path = self.bin / name
        write_lf(path, body)
        path.chmod(0o755)

    def env(self, state):
        env = dict(os.environ)
        env.pop("STUB_PRINT_JSON", None)
        env.update({
            "KLIPPER_PATH": self.klipper.as_posix(),
            "MOONRAKER_PATH": self.moonraker.as_posix(),
            "PRINTER_DATA": self.printer_data.as_posix(),
            "INSTALL_PATH": REPO.as_posix(),
            "PYTHON_BIN": Path(sys.executable).as_posix(),
            "PATH": str(self.bin) + os.pathsep + os.environ["PATH"],
            "MSYS": "winsymlinks:nativestrict",
            "NO_COLOR": "1",
            "STUB_LOG": self.log.as_posix(),
        })
        if state is not None:
            env["STUB_PRINT_JSON"] = state
        return env

    def run(self, script, answers="", state=STATE_STANDBY):
        """Run a script from the repo root. Returns a CompletedProcess."""
        # Bytes in and out, so no text layer adds carriage returns on Windows.
        done = subprocess.run(
            [shutil.which("bash"), (REPO / script).as_posix()],
            input=answers.encode("utf-8"),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=self.env(state), cwd=str(self.root))
        return subprocess.CompletedProcess(
            done.args, done.returncode,
            done.stdout.decode("utf-8", "replace"),
            done.stderr.decode("utf-8", "replace"))

    def log_text(self):
        return read_text(self.log)

    def snapshot(self):
        """Every file under printer_data, plus the link targets, by content."""
        snap = {}
        for path in sorted(self.printer_data.rglob("*")):
            if path.is_file():
                snap[path.relative_to(self.root).as_posix()] = path.read_bytes()
        for link in (self.extras / "heater_virtual.py",
                     self.components / "iheater_remote.py"):
            snap[link.name] = os.readlink(str(link)) if link.is_symlink() else None
        return snap


class ScriptTestBase(unittest.TestCase):
    """Builds a Fixture for each test. Skips when bash cannot be used."""

    @classmethod
    def setUpClass(cls):
        if cls is ScriptTestBase:
            raise unittest.SkipTest("base class")
        if not bash_is_usable():
            raise unittest.SkipTest("needs a POSIX bash and working symlinks")

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.fx = Fixture(Path(tmp.name).resolve())

    def run_ok(self, script, answers="", state=STATE_STANDBY):
        result = self.fx.run(script, answers, state)
        self.assertEqual(
            result.returncode, 0,
            "exit %s\nstdout:\n%s\nstderr:\n%s"
            % (result.returncode, result.stdout, result.stderr))
        return result


class InstallSmokeTests(ScriptTestBase):
    def install(self, answers="", state=STATE_STANDBY):
        return self.run_ok("install.sh", answers, state)

    def installed_cfg(self):
        return read_text(self.fx.addon_cfg).replace("\r\n", "\n")

    def test_fresh_install(self):
        self.install()
        fx = self.fx
        link = fx.extras / "heater_virtual.py"
        self.assertTrue(link.is_symlink())
        self.assertEqual(Path(os.path.realpath(str(link))),
                         (REPO / "klipper" / "heater_virtual.py").resolve())
        link = fx.components / "iheater_remote.py"
        self.assertTrue(link.is_symlink())
        self.assertEqual(Path(os.path.realpath(str(link))),
                         (REPO / "moonraker" / "iheater_remote.py").resolve())

        cfg = self.installed_cfg()
        self.assertIn("# CONFIG_VERSION: 1", cfg)
        self.assertIn("#remote_token:", cfg)

        self.assertEqual(
            read_text(fx.printer_cfg).splitlines()[0],
            "[include addons/iheater_remote.cfg]")

        conf = read_text(fx.moonraker_conf)
        self.assertIn("[iheater_remote]", conf)
        self.assertIn("[update_manager iheater-remote]", conf)
        origin = subprocess.run(
            ["git", "-C", str(REPO), "remote", "get-url", "origin"],
            stdout=subprocess.PIPE, universal_newlines=True,
            check=True).stdout.strip()
        self.assertIn("path: " + REPO.as_posix() + "\n", conf)
        self.assertIn("origin: " + origin + "\n", conf)

        self.assertEqual(fx.log_text(), "restart moonraker\nrestart klipper\n")

    def test_rerun_changes_nothing(self):
        self.install()
        before = self.fx.snapshot()
        log = self.fx.log_text()
        result = self.install()
        self.assertEqual(self.fx.snapshot(), before)
        self.assertEqual(self.fx.log_text(), log)
        self.assertIn("no restart needed", result.stdout)

    def test_install_sets_token(self):
        self.install("abcd-efgh\n\n")
        self.assertIn("\nremote_token: abcd-efgh\n", self.installed_cfg())

    def test_install_rejects_bad_token_then_accepts_blank(self):
        result = self.install("bad\n\n\n")
        self.assertIn("#remote_token:", self.installed_cfg())
        self.assertNotIn("\nremote_token:", self.installed_cfg())
        self.assertIn("not a pairing token", result.stdout)

    def test_install_while_printing(self):
        result = self.install(state=STATE_PRINTING)
        self.assertEqual(self.fx.log_text(), "restart moonraker\n")
        self.assertIn("FIRMWARE_RESTART", result.stdout)

    def test_install_state_unreadable(self):
        self.install(state=None)
        self.assertNotIn("restart klipper", self.fx.log_text())

    def test_existing_macro_blocks_include(self):
        with open(str(self.fx.printer_cfg), "a", encoding="utf-8",
                  newline="\n") as handle:
            handle.write("[gcode_macro M141]\ngcode:\n")
        result = self.install()
        self.assertNotIn("[include addons/iheater_remote.cfg]",
                         read_text(self.fx.printer_cfg))
        self.assertRegex(result.stdout, r"printer\.cfg:\d+")

    def test_no_answer_declines_include(self):
        # Token and sensor prompts take blanks, then "No" at the include.
        self.install("\n\nNo\n")
        self.assertNotIn("[include addons/iheater_remote.cfg]",
                         read_text(self.fx.printer_cfg))

    def test_unclear_answer_asks_again(self):
        result = self.install("\n\nmaybe\nn\n")
        self.assertNotIn("[include addons/iheater_remote.cfg]",
                         read_text(self.fx.printer_cfg))
        self.assertEqual(result.stderr.count("to printer.cfg?"), 2)

    def test_symlinked_macro_file_blocks_include(self):
        outside = self.fx.root / "elsewhere"
        outside.mkdir()
        write_lf(outside / "real_macros.cfg", "[gcode_macro M191]\ngcode:\n")
        os.symlink(str(outside / "real_macros.cfg"),
                   str(self.fx.config_dir / "macros.cfg"))
        result = self.install()
        self.assertNotIn("[include addons/iheater_remote.cfg]",
                         read_text(self.fx.printer_cfg))
        self.assertRegex(result.stdout, r"macros\.cfg:\d+")

    def edit_installed_to_old(self):
        text = self.installed_cfg()
        text = text.replace("# CONFIG_VERSION: 1\n", "")
        text = text.replace("#remote_token: XXXX-XXXX",
                            "remote_token: ABCD-EFGH")
        text = text.replace("sensor: temperature_sensor chamber_hexa",
                            "sensor: temperature_sensor other")
        write_lf(self.fx.addon_cfg, text)
        return text

    def test_upgrade_old_config(self):
        self.install()
        edited = self.edit_installed_to_old()
        self.install("y\n\n")
        cfg = self.installed_cfg()
        self.assertIn("# CONFIG_VERSION: 1", cfg)
        self.assertIn("\nremote_token: ABCD-EFGH\n", cfg)
        self.assertIn("\nsensor: temperature_sensor other\n", cfg)
        backups = glob.glob(str(self.fx.addon_cfg) + ".bak-*")
        self.assertEqual(len(backups), 1)
        self.assertEqual(read_text(backups[0]), edited)

    def test_upgrade_declined_keeps_file(self):
        self.install()
        edited = self.edit_installed_to_old()
        result = self.install("\n")
        self.assertEqual(self.installed_cfg(), edited)
        self.assertIn("kept your older config", result.stdout)
        self.assertEqual(glob.glob(str(self.fx.addon_cfg) + ".bak-*"), [])

    def test_install_without_origin(self):
        self.fx.add_stub("git", "#!/bin/sh\nexit 1\n")
        self.install()
        conf = read_text(self.fx.moonraker_conf)
        self.assertIn("[iheater_remote]", conf)
        self.assertNotIn("[update_manager iheater-remote]", conf)

    def test_regular_file_in_extras_is_moved_aside(self):
        regular = self.fx.extras / "heater_virtual.py"
        write_lf(regular, "# somebody's copy\n")
        self.install()
        self.assertTrue(regular.is_symlink())
        backups = glob.glob(str(regular) + ".bak-*")
        self.assertEqual(len(backups), 1)
        self.assertEqual(read_text(backups[0]), "# somebody's copy\n")


class UninstallSmokeTests(ScriptTestBase):
    def install(self, answers="", state=STATE_STANDBY):
        return self.run_ok("install.sh", answers, state)

    def uninstall(self, answers="", state=STATE_STANDBY):
        return self.run_ok("uninstall.sh", answers, state)

    def write_bytes(self, path, text):
        with open(str(path), "wb") as handle:
            handle.write(text.encode("utf-8"))

    def check_restores(self, newline):
        fx = self.fx
        self.write_bytes(
            fx.printer_cfg,
            "[printer]\n#*# <---- SAVE_CONFIG ---->\n"
            "#*# DO NOT EDIT THIS BLOCK OR BELOW.\n".replace("\n", newline))
        self.write_bytes(
            fx.moonraker_conf,
            "[server]\nhost: 0.0.0.0\n".replace("\n", newline))
        printer_before = fx.printer_cfg.read_bytes()
        conf_before = fx.moonraker_conf.read_bytes()

        self.install()
        self.assertNotEqual(fx.printer_cfg.read_bytes(), printer_before)
        self.assertNotEqual(fx.moonraker_conf.read_bytes(), conf_before)
        self.uninstall()

        self.assertEqual(fx.printer_cfg.read_bytes(), printer_before)
        self.assertEqual(fx.moonraker_conf.read_bytes(), conf_before)
        self.assertFalse((fx.extras / "heater_virtual.py").exists())
        self.assertFalse((fx.extras / "heater_virtual.py").is_symlink())
        self.assertFalse((fx.components / "iheater_remote.py").is_symlink())
        self.assertTrue(fx.addon_cfg.is_file())
        self.assertTrue(fx.log_text().endswith(
            "restart moonraker\nrestart klipper\n"))

    def test_uninstall_restores_files(self):
        self.check_restores("\n")

    def test_uninstall_restores_crlf_files(self):
        self.check_restores("\r\n")

    def test_uninstall_deletes_cfg_on_yes(self):
        self.install()
        self.uninstall("\ny\n\n")
        self.assertFalse(self.fx.addon_cfg.exists())
        self.assertEqual(
            len(glob.glob(str(self.fx.addon_cfg) + ".bak-*")), 1)

    def test_uninstall_leaves_glob_include(self):
        write_lf(self.fx.printer_cfg,
                 "[include addons/*.cfg]\n[printer]\n")
        self.install()
        result = self.uninstall()
        self.assertIn("[include addons/*.cfg]\n",
                      read_text(self.fx.printer_cfg))
        self.assertIn("a glob include covers it", result.stdout)

    def test_uninstall_leaves_foreign_link(self):
        outside = self.fx.root / "elsewhere"
        outside.mkdir()
        write_lf(outside / "mine.py", "# not ours\n")
        link = self.fx.extras / "heater_virtual.py"
        os.symlink(str(outside / "mine.py"), str(link))
        self.install("n\n")
        self.uninstall()
        self.assertTrue(link.is_symlink())
        self.assertEqual(os.path.realpath(str(link)),
                         os.path.realpath(str(outside / "mine.py")))
        # Our own component link is still removed.
        self.assertFalse((self.fx.components / "iheater_remote.py").is_symlink())

    def test_uninstall_twice(self):
        self.install()
        self.uninstall()
        before = self.fx.snapshot()
        log = self.fx.log_text()
        result = self.uninstall()
        self.assertEqual(self.fx.snapshot(), before)
        self.assertEqual(self.fx.log_text(), log)
        self.assertIn("no restart needed", result.stdout)


if __name__ == "__main__":
    unittest.main()
