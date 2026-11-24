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

SENSOR_SECTION = ("[temperature_sensor chamber_hexa]\n"
                  "sensor_type: temperature_host\n")
PRINTER_CFG_TEXT = (
    "[printer]\n" + SENSOR_SECTION + "#*# <---- SAVE_CONFIG ---->\n"
    "#*# DO NOT EDIT THIS BLOCK OR BELOW.\n")

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
        "  reload)\n"
        '    echo "reload $2" >> "$STUB_LOG"\n'
        "    ;;\n"
        "esac\n"
    ),
    # Not installed by default: tests that want nginx present add it.
    "nginx": (
        "#!/bin/sh\n"
        'echo "nginx $*" >> "$STUB_LOG"\n'
        'if [ "$1" = "-t" ] && [ -n "${STUB_NGINX_FAIL:-}" ]; then\n'
        "  exit 1\n"
        "fi\n"
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


def write_old_layout(fx):
    """The manual layout from before install.sh: cfg beside printer.cfg."""
    text = read_text(REPO / "klipper" / "iheater_remote.cfg")
    text = text.replace("\r\n", "\n").replace("# CONFIG_VERSION: 1\n", "")
    text = text.replace("#remote_token: XXXX-XXXX", "remote_token: 7K3M9QX2")
    write_lf(fx.config_dir / "iheater_remote.cfg", text)
    with open(str(fx.printer_cfg), "a", encoding="utf-8", newline="\n") as f:
        f.write("[include iheater_remote.cfg]\n")
    return text


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
        self.nginx = self.root / "nginx"
        self.sbin = self.root / "sbin"  # empty, so no real /usr/sbin/nginx
        self.sbin.mkdir()
        self.extra_env = {}

        self.extras.mkdir(parents=True)
        self.components.mkdir(parents=True)
        self.config_dir.mkdir(parents=True)
        self.bin.mkdir()
        write_lf(self.printer_cfg, PRINTER_CFG_TEXT)
        write_lf(self.moonraker_conf, "[server]\nhost: 0.0.0.0\n")
        write_lf(self.log, "")
        for name, body in STUBS.items():
            if name != "nginx":
                self.add_stub(name, body)

    def add_stub(self, name, body):
        path = self.bin / name
        write_lf(path, body)
        path.chmod(0o755)

    def env(self, state):
        env = dict(os.environ)
        env.pop("STUB_PRINT_JSON", None)
        env.pop("STUB_NGINX_FAIL", None)
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
            "NGINX_DIR": self.nginx.as_posix(),
            "NGINX_SBIN": self.sbin.as_posix(),
        })
        env.update(self.extra_env)
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
        for link in (self.extras / "heater_iheater_remote.py",
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
        link = fx.extras / "heater_iheater_remote.py"
        self.assertTrue(link.is_symlink())
        self.assertEqual(Path(os.path.realpath(str(link))),
                         (REPO / "klipper" / "heater_iheater_remote.py").resolve())
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

    def test_save_config_backup_not_flagged(self):
        write_lf(self.fx.config_dir / "printer-20260101_120000.cfg",
                 "[gcode_macro M141]\ngcode:\n")
        result = self.install()
        self.assertNotIn("defines [gcode_macro", result.stdout)
        self.assertIn("[include addons/iheater_remote.cfg]",
                      read_text(self.fx.printer_cfg))

    def test_missing_sensor_warns_and_skips_restart(self):
        write_lf(self.fx.printer_cfg, "[printer]\n")
        result = self.install()
        self.assertIn("No [temperature_sensor chamber_hexa] section found",
                      result.stdout)
        self.assertIn("sensor:", result.stdout)
        self.assertEqual(self.fx.log_text(), "restart moonraker\n")

    def test_chosen_sensor_is_checked(self):
        result = self.install("\nsensor.x+y\n")
        self.assertIn("No [sensor.x+y] section found", result.stdout)
        self.assertNotIn("restart klipper", self.fx.log_text())

    def test_install_migrates_old_layout(self):
        old = write_old_layout(self.fx)
        # Move: yes. Upgrade to version 1: yes.
        result = self.install("y\ny\n")
        fx = self.fx
        self.assertIn("\nremote_token: 7K3M9QX2\n", self.installed_cfg())
        self.assertFalse((fx.config_dir / "iheater_remote.cfg").exists())
        backups = glob.glob(str(fx.config_dir / "iheater_remote.cfg.bak-*"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(read_text(backups[0]), old)
        printer = read_text(fx.printer_cfg)
        self.assertIn("[include addons/iheater_remote.cfg]\n", printer)
        self.assertNotIn("[include iheater_remote.cfg]", printer)
        self.assertEqual(len(glob.glob(str(fx.printer_cfg) + ".bak-*")), 1)
        self.assertNotIn("no pairing token yet", result.stdout)
        self.assertNotIn("defines [gcode_macro", result.stdout)

    def test_install_old_layout_declined(self):
        old = write_old_layout(self.fx)
        before = read_text(self.fx.printer_cfg)
        # Move: no. Then blank token, blank sensor, default at the include.
        result = self.install("n\n\n\n\n")
        fx = self.fx
        self.assertEqual(read_text(fx.config_dir / "iheater_remote.cfg"), old)
        self.assertEqual(read_text(fx.printer_cfg), before)
        self.assertIn("older copy", result.stdout)
        self.assertNotIn("defines [gcode_macro", result.stdout)

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
        regular = self.fx.extras / "heater_iheater_remote.py"
        write_lf(regular, "# somebody's copy\n")
        self.install()
        self.assertTrue(regular.is_symlink())
        backups = glob.glob(str(regular) + ".bak-*")
        self.assertEqual(len(backups), 1)
        self.assertEqual(read_text(backups[0]), "# somebody's copy\n")


class NginxStepTests(ScriptTestBase):
    ANSWERS = ("y\nheater.example.com\n/etc/ssl/h.pem\n/etc/ssl/h.key\n"
               "192.168.1.5\n")

    def setUp(self):
        super().setUp()
        fx = self.fx
        # A first run does the Klipper work (nginx is not on PATH yet), so the
        # runs under test ask only the nginx questions.
        self.run_ok("install.sh")
        fx.add_stub("nginx", STUBS["nginx"])
        (fx.nginx / "sites-available").mkdir(parents=True)
        (fx.nginx / "sites-enabled").mkdir()
        self.site = fx.nginx / "sites-available" / "iheater-remote"
        self.link = fx.nginx / "sites-enabled" / "iheater-remote"
        self.other = fx.nginx / "sites-available" / "mainsail"
        write_lf(self.other, "server { listen 80; }\n")
        write_lf(fx.log, "")

    def nginx_files(self):
        return sorted(p.relative_to(self.fx.nginx).as_posix()
                      for p in self.fx.nginx.rglob("*"))

    def test_nginx_absent_skips_quietly(self):
        (self.fx.bin / "nginx").unlink()
        result = self.run_ok("install.sh", self.ANSWERS)
        self.assertNotIn("nginx", result.stdout + result.stderr)
        self.assertFalse(self.site.exists())
        self.assertFalse(os.path.lexists(str(self.link)))

    def test_nginx_found_in_sbin_off_path(self):
        (self.fx.bin / "nginx").unlink()
        stub = self.fx.sbin / "nginx"
        write_lf(stub, STUBS["nginx"])
        stub.chmod(0o755)
        result = self.run_ok("install.sh", self.ANSWERS)
        self.assertIn("HTTPS nginx site", result.stderr)
        self.assertIn("heater.example.com", read_text(self.site))
        self.assertTrue(os.path.lexists(str(self.link)))
        self.assertEqual(self.fx.log_text(), "nginx -t\nreload nginx\n")

    def test_nginx_declined(self):
        self.run_ok("install.sh", "n\n")
        self.assertFalse(self.site.exists())
        self.assertFalse(os.path.lexists(str(self.link)))
        self.assertEqual(self.fx.log_text(), "")

    def test_nginx_blank_answer_skips(self):
        self.run_ok("install.sh", "")
        self.assertFalse(self.site.exists())

    def test_nginx_blank_value_skips(self):
        result = self.run_ok("install.sh", "y\nheater\n\n")
        self.assertFalse(self.site.exists())
        self.assertIn("left blank", result.stdout)

    def test_nginx_site_written_and_reloaded(self):
        self.run_ok("install.sh", self.ANSWERS)
        text = read_text(self.site)
        for value in ("server_name heater.example.com;",
                      "ssl_certificate /etc/ssl/h.pem;",
                      "ssl_certificate_key /etc/ssl/h.key;",
                      "proxy_pass http://192.168.1.5;",
                      "proxy_pass http://192.168.1.5:81/;"):
            self.assertIn(value, text)
        for placeholder in ("@SERVER_NAME@", "@CERT@", "@KEY@", "@REMOTE@"):
            self.assertNotIn(placeholder, text)
        self.assertIn("client_max_body_size 4m;", text)
        self.assertTrue(os.path.lexists(str(self.link)))
        self.assertEqual(self.fx.log_text(), "nginx -t\nreload nginx\n")

    def test_nginx_values_are_substituted_literally(self):
        self.run_ok("install.sh",
                    "y\nh.example\n/a&b/c|d.pem\n/k/REMOTE.key\nrem\n")
        text = read_text(self.site)
        self.assertIn("ssl_certificate /a&b/c|d.pem;", text)
        self.assertIn("ssl_certificate_key /k/REMOTE.key;", text)
        self.assertIn("proxy_pass http://rem;", text)

    def test_nginx_unsafe_value_is_asked_again(self):
        result = self.run_ok(
            "install.sh", "y\nbad name\nx;y\nheater\n/c.pem\n/k.key\nrem\n")
        text = read_text(self.site)
        self.assertIn("server_name heater;", text)
        self.assertNotIn("bad name", text)
        self.assertNotIn("x;y", text)
        self.assertIn("try again", result.stdout)

    def test_nginx_every_unsafe_character_is_refused(self):
        for value in ("a b", "a;b", "a{b", "a}b", 'a"b', "a'b", "a$b", "a#b",
                      "a\\b", "a\tb"):
            with self.subTest(value=value):
                # The unsafe value, then a blank answer to give up.
                self.run_ok("install.sh", "y\n%s\n\n" % value)
                self.assertFalse(self.site.exists())

    def test_nginx_existing_site_backed_up(self):
        write_lf(self.site, "old\n")
        self.run_ok("install.sh", self.ANSWERS)
        backups = glob.glob(str(self.site) + ".bak-*")
        self.assertEqual(len(backups), 1)
        self.assertEqual(read_text(backups[0]), "old\n")
        self.assertIn("heater.example.com", read_text(self.site))

    def test_nginx_test_failure_rolls_back(self):
        self.fx.extra_env["STUB_NGINX_FAIL"] = "1"
        result = self.run_ok("install.sh", self.ANSWERS)
        self.assertEqual(self.fx.log_text(), "nginx -t\n")
        self.assertIn("rolled back", result.stdout)
        self.assertEqual(self.nginx_files(),
                         ["sites-available", "sites-available/mainsail",
                          "sites-enabled"])

    def test_nginx_test_failure_restores_old_site(self):
        write_lf(self.site, "old\n")
        self.fx.extra_env["STUB_NGINX_FAIL"] = "1"
        self.run_ok("install.sh", self.ANSWERS)
        self.assertEqual(read_text(self.site), "old\n")
        self.assertFalse(os.path.lexists(str(self.link)))
        self.assertNotIn("reload", self.fx.log_text())

    def test_nginx_test_failure_keeps_existing_link(self):
        write_lf(self.site, "old\n")
        os.symlink(str(self.site), str(self.link))
        self.fx.extra_env["STUB_NGINX_FAIL"] = "1"
        self.run_ok("install.sh", self.ANSWERS)
        self.assertTrue(os.path.islink(str(self.link)))
        self.assertEqual(read_text(self.site), "old\n")

    def test_nginx_never_touches_other_sites(self):
        before = self.other.read_bytes()
        self.run_ok("install.sh", self.ANSWERS)
        self.assertEqual(self.other.read_bytes(), before)
        self.assertEqual(self.nginx_files(),
                         ["sites-available", "sites-available/iheater-remote",
                          "sites-available/mainsail", "sites-enabled",
                          "sites-enabled/iheater-remote"])


class UninstallNginxTests(ScriptTestBase):
    """uninstall.sh removes the site install.sh wrote, and nothing else."""

    # Uninstall asks: remove the include (y), delete the config (n), then the
    # nginx site. Two empty lines take the first two defaults.
    KEEP_DEFAULTS = "\n\n"

    def setUp(self):
        super().setUp()
        fx = self.fx
        self.run_ok("install.sh")
        fx.add_stub("nginx", STUBS["nginx"])
        (fx.nginx / "sites-available").mkdir(parents=True)
        (fx.nginx / "sites-enabled").mkdir()
        self.site = fx.nginx / "sites-available" / "iheater-remote"
        self.link = fx.nginx / "sites-enabled" / "iheater-remote"
        self.other = fx.nginx / "sites-available" / "mainsail"
        write_lf(self.other, "server { listen 80; }\n")
        self.run_ok("install.sh", NginxStepTests.ANSWERS)
        self.assertTrue(os.path.lexists(str(self.link)))
        write_lf(fx.log, "")

    def nginx_files(self):
        return sorted(p.relative_to(self.fx.nginx).as_posix()
                      for p in self.fx.nginx.rglob("*")
                      if ".bak-" not in p.name)

    def test_site_and_link_removed_and_nginx_reloaded(self):
        site_text = read_text(self.site)
        result = self.run_ok("uninstall.sh", self.KEEP_DEFAULTS + "\n")
        self.assertFalse(self.site.exists())
        self.assertFalse(os.path.lexists(str(self.link)))
        self.assertEqual(self.nginx_files(),
                         ["sites-available", "sites-available/mainsail",
                          "sites-enabled"])
        backups = glob.glob(str(self.site) + ".bak-*")
        self.assertEqual(len(backups), 1)
        self.assertEqual(read_text(backups[0]), site_text)
        self.assertTrue(self.fx.log_text().startswith(
            "nginx -t\nreload nginx\n"))
        self.assertIn("removed the iheater-remote nginx site", result.stdout)

    def test_declined_keeps_site(self):
        self.run_ok("uninstall.sh", self.KEEP_DEFAULTS + "n\n")
        self.assertTrue(self.site.exists())
        self.assertTrue(os.path.islink(str(self.link)))
        self.assertNotIn("nginx", self.fx.log_text())

    def test_site_not_written_by_install_is_left(self):
        write_lf(self.site, "server { listen 443; }\n")
        result = self.run_ok("uninstall.sh", self.KEEP_DEFAULTS + "\n")
        self.assertEqual(read_text(self.site), "server { listen 443; }\n")
        self.assertTrue(os.path.islink(str(self.link)))
        self.assertIn("left it", result.stdout)
        self.assertNotIn("nginx", self.fx.log_text())

    def test_regular_file_in_sites_enabled_is_left(self):
        self.link.unlink()
        write_lf(self.link, "server { listen 443; }\n")
        result = self.run_ok("uninstall.sh", self.KEEP_DEFAULTS + "\n")
        self.assertTrue(self.site.exists())
        self.assertTrue(self.link.is_file())
        self.assertIn("left", result.stdout)

    def test_nginx_test_failure_puts_site_back(self):
        site_text = read_text(self.site)
        self.fx.extra_env["STUB_NGINX_FAIL"] = "1"
        result = self.run_ok("uninstall.sh", self.KEEP_DEFAULTS + "\n")
        self.assertEqual(read_text(self.site), site_text)
        self.assertTrue(os.path.islink(str(self.link)))
        self.assertNotIn("reload nginx", self.fx.log_text())
        self.assertIn("put back", result.stdout)

    def test_dangling_link_alone_is_removed(self):
        self.site.unlink()
        self.run_ok("uninstall.sh", self.KEEP_DEFAULTS + "\n")
        self.assertFalse(os.path.lexists(str(self.link)))
        self.assertTrue(self.fx.log_text().startswith(
            "nginx -t\nreload nginx\n"))

    def test_nginx_not_installed_still_removes_site(self):
        (self.fx.bin / "nginx").unlink()
        result = self.run_ok("uninstall.sh", self.KEEP_DEFAULTS + "\n")
        self.assertFalse(self.site.exists())
        self.assertFalse(os.path.lexists(str(self.link)))
        self.assertNotIn("nginx", self.fx.log_text())
        self.assertIn("reload nginx", result.stdout)

    def test_other_sites_untouched(self):
        before = self.other.read_bytes()
        self.run_ok("uninstall.sh", self.KEEP_DEFAULTS + "\n")
        self.assertEqual(self.other.read_bytes(), before)


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
            PRINTER_CFG_TEXT.replace("\n", newline))
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
        self.assertFalse((fx.extras / "heater_iheater_remote.py").exists())
        self.assertFalse((fx.extras / "heater_iheater_remote.py").is_symlink())
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
                 "[include addons/*.cfg]\n" + PRINTER_CFG_TEXT)
        self.install()
        log_before = self.fx.log_text()
        result = self.uninstall()
        self.assertIn("[include addons/*.cfg]\n",
                      read_text(self.fx.printer_cfg))
        self.assertIn("config error", result.stdout)
        self.assertIn("addons/iheater_remote.cfg", result.stdout)
        added = self.fx.log_text()[len(log_before):]
        self.assertNotIn("restart klipper", added)
        self.assertIn("restart moonraker", added)

    def test_uninstall_declined_include_does_not_restart_klipper(self):
        self.install()
        log_before = self.fx.log_text()
        result = self.uninstall("n\n")
        self.assertIn("[include addons/iheater_remote.cfg]\n",
                      read_text(self.fx.printer_cfg))
        self.assertIn("config error", result.stdout)
        added = self.fx.log_text()[len(log_before):]
        self.assertNotIn("restart klipper", added)
        self.assertIn("restart moonraker", added)

    def test_uninstall_old_layout_left_in_place(self):
        write_old_layout(self.fx)
        self.install("n\n\n\n\n")
        log_before = self.fx.log_text()
        result = self.uninstall()
        self.assertIn("config error", result.stdout)
        self.assertIn("iheater_remote.cfg", result.stdout)
        added = self.fx.log_text()[len(log_before):]
        self.assertNotIn("restart klipper", added)
        self.assertIn("restart moonraker", added)

    def test_uninstall_leaves_foreign_link(self):
        outside = self.fx.root / "elsewhere"
        outside.mkdir()
        write_lf(outside / "mine.py", "# not ours\n")
        link = self.fx.extras / "heater_iheater_remote.py"
        os.symlink(str(outside / "mine.py"), str(link))
        self.install("n\n")
        self.uninstall()
        self.assertTrue(link.is_symlink())
        self.assertEqual(os.path.realpath(str(link)),
                         os.path.realpath(str(outside / "mine.py")))
        # Our own component link is still removed.
        self.assertFalse((self.fx.components / "iheater_remote.py").is_symlink())

    def test_uninstall_unresolvable_link(self):
        self.install()
        link = self.fx.extras / "heater_iheater_remote.py"
        link.unlink()
        os.symlink(str(self.fx.root / "gone" / "deeper" / "heater_iheater_remote.py"),
                   str(link))
        result = self.uninstall()
        self.assertTrue(link.is_symlink())
        self.assertFalse((self.fx.components / "iheater_remote.py").is_symlink())
        self.assertIn("left it", result.stdout)
        self.assertTrue(self.fx.log_text().endswith(
            "restart moonraker\nrestart klipper\n"))

    def test_uninstall_without_nginx_site_is_quiet(self):
        (self.fx.nginx / "sites-available").mkdir(parents=True)
        (self.fx.nginx / "sites-enabled").mkdir()
        self.fx.add_stub("nginx", STUBS["nginx"])
        self.install()
        result = self.uninstall()
        self.assertNotIn("nginx", result.stdout + result.stderr)
        self.assertNotIn("nginx", self.fx.log_text())

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
