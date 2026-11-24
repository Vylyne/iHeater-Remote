"""Tests for the login page's script (kAuthJs in src/standalone/AuthScript.h).

The script is a browser copy of lib/remote-policy/src/AuthCrypto, so it must
give byte-identical results. The vectors match test/test_auth_crypto.

Run: python -m unittest discover -s scripts -p "test_auth_js.py" -v
"""
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(REPO, "src", "standalone", "AuthScript.h")

HARNESS = r"""
const fs = require("fs");
const vm = require("vm");
vm.runInThisContext(fs.readFileSync(process.argv[1], "utf8"));
const I = globalThis.IHA;
const u = (s) => I.utf8(s);
const rep = (b, n) => new Uint8Array(n).fill(b);
const h = (x) => I.hex(x);
const out = {
  sha_empty: h(I.sha256(u(""))),
  sha_abc: h(I.sha256(u("abc"))),
  sha_two_block: h(I.sha256(u(
    "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
  sha_million_a: h(I.sha256(rep(0x61, 1000000))),
  sha_55_a: h(I.sha256(rep(0x61, 55))),
  sha_63_a: h(I.sha256(rep(0x61, 63))),
  hmac_1: h(I.hmac(rep(0x0b, 20), u("Hi There"))),
  hmac_2: h(I.hmac(u("Jefe"), u("what do ya want for nothing?"))),
  hmac_6: h(I.hmac(rep(0xaa, 131), u(
    "Test Using Larger Than Block-Size Key - Hash Key First"))),
  pbkdf2_1: h(I.pbkdf2(u("password"), u("salt"), 1, 32)),
  pbkdf2_2: h(I.pbkdf2(u("password"), u("salt"), 2, 32)),
  pbkdf2_4096: h(I.pbkdf2(u("password"), u("salt"), 4096, 32)),
  pbkdf2_40: h(I.pbkdf2(u("passwordPASSWORDpassword"),
    u("saltSALTsaltSALTsaltSALTsaltSALTsalt"), 4096, 40)),
  pbkdf2_long_pw: h(I.pbkdf2(rep(0x61, 65), u("salt"), 2, 32)),
  unhex: Array.from(I.unhex("00ABff")),
  login: typeof I.login,
  setPassword: typeof I.setPassword,
};
console.log(JSON.stringify(out));
"""


def extract_script():
    with open(HEADER, "r", encoding="utf-8") as f:
        text = f.read()
    start = text.index('R"JS(') + len('R"JS(')
    end = text.index(')JS"', start)
    return text[start:end]


@unittest.skipIf(shutil.which("node") is None, "node is not installed")
class AuthJsTest(unittest.TestCase):
    def test_vectors_match_the_firmware(self):
        script = extract_script()
        with tempfile.TemporaryDirectory() as tmp:
            script_path = os.path.join(tmp, "auth.js")
            with open(script_path, "w", encoding="utf-8", newline="\n") as f:
                f.write(script)
            run = subprocess.run(
                ["node", "-e", HARNESS, script_path],
                capture_output=True, text=True, timeout=120)
        self.assertEqual(run.returncode, 0, run.stderr)
        got = json.loads(run.stdout)
        a = b"a"
        expected = {
            "sha_empty": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "sha_abc": "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "sha_two_block": "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
            "sha_million_a": "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
            "sha_55_a": hashlib.sha256(a * 55).hexdigest(),
            "sha_63_a": hashlib.sha256(a * 63).hexdigest(),
            "hmac_1": "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
            "hmac_2": "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
            "hmac_6": "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
            "pbkdf2_1": "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b",
            "pbkdf2_2": "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43",
            "pbkdf2_4096": "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a",
            "pbkdf2_40": "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1c635518c7dac47e9",
            "pbkdf2_long_pw": hashlib.pbkdf2_hmac(
                "sha256", a * 65, b"salt", 2, 32).hex(),
            "unhex": [0x00, 0xAB, 0xFF],
            "login": "function",
            "setPassword": "function",
        }
        for key, want in expected.items():
            with self.subTest(key=key):
                self.assertEqual(got[key], want)

    def test_script_is_ascii(self):
        script = extract_script()
        self.assertTrue(all(b < 0x80 for b in script.encode("utf-8")))
        self.assertNotIn("\r", script)


if __name__ == "__main__":
    unittest.main()
