# Manual override and web login — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** make a manual press while following Klipper a temporary override
instead of an unbind. Then lock every web action and the `/setup` and `/fw`
pages behind a login: password challenge, session cookie, origin check,
trusted proxies, authentik header login, proxy-only mode and a BOOT reset.

**Architecture:**
- **Pure parts in `lib/remote-policy`, natively tested:**
  - the override (`KlipperOverride`);
  - the crypto (`AuthCrypto`): SHA-256, HMAC and PBKDF2;
  - the stateless request decisions (`AuthPolicy`);
  - the stateful tables (`AuthState`): nonces, lockout, sessions and
    running hours.
- **Firmware in `src/standalone`:**
  - `AuthStore` keeps the auth settings in their own NVS namespace;
  - `StandaloneApp.cpp` wraps every route in one gate and adds the
    `/api/auth/*` endpoints;
  - the pages share one embedded `auth.js` holding the browser copy of the
    crypto.
- **nginx:** an optional `install.sh` step and a README snippet.

**Tech stack:**
- Arduino-ESP32 3.3.12 (pioarduino 55.03.312-1): `WebServer`,
  `WebSocketsServer`, `Preferences`.
- ArduinoJson 6.21 and Unity for the native tests.
- Plain ES2017 JavaScript, checked with Node from a Python unittest.
- bash for the installer.

**Specs:**
- `docs/design/2026-11-23-manual-override-design.md` (Task 1);
- `docs/design/2026-11-23-web-auth-design.md` (Tasks 2–11).

Executors read the spec for their task.

**Branch:** `feat/web-auth`.

## Global Constraints

- **Gates.** All five pass before every commit (AGENTS.md):
  - Build: `pio run -e esp32c3-super-mini-remote`, from a shell without
    `MSYSTEM` (PowerShell or cmd). This session uses
    `powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$TEMP/pio-nomsys.ps1")" run -e esp32c3-super-mini-remote`.
  - Native tests:
    `export PATH="$HOME/scoop/apps/mingw/current/bin:$PATH"; pio test -e native`
  - Python: `python -m unittest discover -s klipper -p "test_*.py"`, then
    the same for `-s moonraker` and `-s scripts`.
- **No new libraries** in `platformio.ini`. Python stays standard-library
  only. Don't change the Python env or pip packages.
- **Writing rules:**
  - Canadian English in comments, docs and page copy.
  - LF line endings.
  - Don't edit `lib/modbus-rtu/`.
- **Pure decisions** go in `lib/remote-policy/` with native tests.
- **Never flash or upload to a device without asking first.** Bench checks
  are the owner's.
- **Commits** end with a blank line, then
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Write them with
  `git commit -m "<subject>" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"`.
- **ArduinoJson 6** stores a `const char*` by pointer. Assign a `String`, or
  a `char[]` that isn't const, for anything built at run time.
- **Secrets:**
  - Never log or return the verifier `V`, a proof or a session token.
  - Never store the password.
  - The Serial log may carry client addresses and header user names.
- **Uploads:** check auth before writing a byte. Both upload routes decide at
  `UPLOAD_FILE_START`. A refused upload calls nothing that touches
  `Update`, the image buffer or the output.
- **Exact values from the spec:**
  - cookie: `iheater_session`, sent as
    `HttpOnly; SameSite=Strict; Path=/; Max-Age=2592000`, plus `; Secure`
    only through a trusted proxy with `X-Forwarded-Proto: https`;
  - session token and nonce: 16 random bytes; up to 4 sessions;
  - session expiry: 720 running hours;
  - nonce life: 60 s, with at most 4 outstanding;
  - lockout: 5 failures in a row, 30 s, a table of 8 clients;
  - trusted proxies: up to 4 IPv4 addresses, and the automatic Moonraker
    entry is on by default;
  - salt: 16 bytes; iterations between 5,000 and 20,000 (this plan uses
    10,000);
  - BOOT reset: a 10 s hold;
  - default header names: `X-authentik-username` and `X-authentik-groups`;
  - the status WebSocket on port 81 stays public.

## Review Focus

1. **Float noise in a repeated Klipper target.** Moonraker re-sends the
   target during an override as, say, 59.99 against a baseline of 60. It
   must still be ignored. Task 1 covers it with
   `test_same_target_within_tolerance_ignored`.
2. **An override before Klipper's first target.** A press lands just after
   boot, while the link is still connecting. The first target that arrives
   becomes the baseline and the override holds; it must not end at once.
   Task 1 covers it with `test_unknown_baseline_adopts_first_target`.
3. **A same-origin GET without `Origin`.** Browsers omit `Origin` on a
   same-origin GET, so `GET /api/auth/challenge` arrives without one. The
   `Referer`, which carries a path and query, must be accepted. Task 4
   covers it with `test_referer_fallback_with_path`.
4. **A `Cookie` header with other apps' cookies.** Mainsail or authentik on
   the same host name add their own. The session must still be found, and a
   look-alike name must not match. Task 4 covers it with
   `test_cookie_among_others` and `test_cookie_lookalike_name_refused`.
5. **A logged-out OTA upload.** It is refused with nothing touched: no
   `Update.begin`, and no `applyMode(0, "ota:start")`. Today that call stops
   pulse heat at `UPLOAD_FILE_START`. Task 6 covers it in Step 4 and its
   bench check.

---

### Task 1: Manual override of the Klipper target

**Files:**
- Create: `lib/remote-policy/src/KlipperOverride.h`, `lib/remote-policy/src/KlipperOverride.cpp`
- Create: `test/test_override/test_main.cpp`
- Modify: `src/standalone/MoonrakerLink.h`, `src/standalone/MoonrakerLink.cpp` (an unpaired callback)
- Modify: `src/standalone/StandaloneApp.cpp`:
  - `applyMode` (:281–326);
  - `onKlipperTarget` (:259);
  - `updateHeatTimer` (:328);
  - `updateLinkSafety` (:480);
  - `statusJson` (:907);
  - `applySetupForm`;
  - `configureRoutes`;
  - `setup()`.
- Modify: `src/standalone/RemoteDashboard.h`

**Interfaces:**
- Produces, used by Tasks 6 and 9:
  ```cpp
  namespace iheaterlink::policy {
  constexpr float kSameTargetToleranceC = 0.05f;
  class KlipperOverride {
   public:
    // A web or button press. Starts an override when klipperSource is true,
    // keeping the baseline of one already running. Returns active().
    bool press(bool klipperSource);
    // A target from Moonraker. Always recorded as Klipper's latest. True
    // when it must be applied now: no override, or a target that ends one.
    bool target(float targetC);
    // Ends any override (Back to Klipper, timer expiry, link-loss cut).
    // Returns Klipper's latest target, or 0 when none has arrived.
    float end();
    // Forgets the override and Klipper's latest target (unpaired, or the
    // source or binding changed).
    void reset();
    bool active() const;
    bool hasKlipperTarget() const;
    float klipperTargetC() const;  // 0 when none
  };
  }
  ```
- Produces: `MoonrakerLink::onUnpaired(UnpairedCallback cb)`, where
  `using UnpairedCallback = void (*)();`. `goUnpaired` calls it before
  `targetCb_(0.0f)`.
- Produces: the route `POST /api/override/end`. It ends the override and
  applies `end()` with source "klipper", then replies with the status JSON.
  With no override running it changes nothing and replies with the status
  JSON.
- Produces: two status JSON fields. `override` is a bool. `klipperTargetC`
  is a number, or `null` when none has arrived.

- [ ] **Step 1: Write the failing tests** in `test/test_override/test_main.cpp`.
  There is one Unity `main` with a `RUN_TEST` for each test, and each test
  uses a fresh `KlipperOverride o;`.
  ```cpp
  test_press_in_manual_source_is_not_override:
    o.target(60); FALSE(o.press(false)); FALSE(o.active()); TRUE(o.target(70));
  test_press_starts_override_with_baseline:
    o.target(60); TRUE(o.press(true)); TRUE(o.active()); FALSE(o.target(60)); TRUE(o.active());
  test_same_target_within_tolerance_ignored:            // Review Focus 1
    o.target(60); o.press(true); FALSE(o.target(59.99f)); FALSE(o.target(60.04f)); TRUE(o.active());
  test_changed_target_ends_override:
    o.target(60); o.press(true); TRUE(o.target(65)); FALSE(o.active()); EQUAL_FLOAT(65, o.klipperTargetC());
  test_change_to_zero_ends_override:
    o.target(60); o.press(true); TRUE(o.target(0)); FALSE(o.active());
  test_second_press_keeps_baseline:
    o.target(60); o.press(true); o.press(true); FALSE(o.target(60)); TRUE(o.target(0));
  test_unknown_baseline_adopts_first_target:             // Review Focus 2
    TRUE(o.press(true)); FALSE(o.hasKlipperTarget()); FALSE(o.target(0)); TRUE(o.active());
    TRUE(o.target(60)); FALSE(o.active());
  test_end_returns_klipper_target:                       // Back to Klipper and timer expiry
    o.target(60); o.press(true); EQUAL_FLOAT(60, o.end()); FALSE(o.active()); TRUE(o.target(60));
  test_end_without_target_returns_zero:
    o.press(true); EQUAL_FLOAT(0, o.end()); FALSE(o.active());
  test_reset_ends_override_and_forgets_target:           // unpaired
    o.target(60); o.press(true); o.reset(); FALSE(o.active()); FALSE(o.hasKlipperTarget());
    EQUAL_FLOAT(0, o.klipperTargetC()); TRUE(o.target(0));
  ```
- [ ] **Step 2: Run them and see them fail.**
  Run: `pio test -e native -f test_override`, with MinGW on `PATH`.
  Expected: a build failure, because `KlipperOverride.h` doesn't exist yet.
- [ ] **Step 3: Implement `KlipperOverride`** in
  `lib/remote-policy/src/KlipperOverride.{h,cpp}`.
  - Equality is `fabsf(a - b) < kSameTargetToleranceC`.
  - A press with no Klipper target yet leaves the baseline unknown. The next
    `target()` sets the baseline and returns false.
- [ ] **Step 4: Run the tests and see them pass.**
  Run: `pio test -e native -f test_override`.
  Expected: 10 tests pass.
- [ ] **Step 5: Wire the override into the firmware.** Add
  `policy::KlipperOverride s_override;` beside `s_settings`.
  - **`applyMode`:** rename `releasePrinter` to `userPress` and replace the
    unbind block with `if (userPress) s_override.press(klipperSource());`.
    The comment says that a press while following Klipper starts an override
    and keeps the pairing.
  - **`onKlipperTarget`:** while `klipperSource()`, call
    `applyIntegrationTarget(t, "klipper", "klipper")` only when
    `s_override.target(t)` is true. Don't log ignored repeats; they arrive
    every 30 s.
  - **`updateHeatTimer`:** on expiry, if `s_override.active()`, call
    `applyIntegrationTarget(s_override.end(), "timer: back to klipper", "klipper")`.
    Otherwise keep `applyMode(0, "timer", false)`.
  - **`updateLinkSafety`:** `heatingFromPrinter` also counts
    `s_override.active()`. Before the cut's `applyIntegrationTarget`, call
    `s_override.end()`. The 10-minute cut then covers override heat too.
  - **`applySetupForm`:** call `s_override.reset()` wherever it calls
    `s_moonraker.configure(...)` or `s_moonraker.stop()`.
  - **`MoonrakerLink`:** add `onUnpaired` with an `unpairedCb_` member.
    `goUnpaired` calls it first.
  - **`setup()`:** register a callback that calls `s_override.reset()`.
  - **`handleOverrideEnd()`:** register it on `/api/override/end`
    (`HTTP_POST`).
  - **`statusJson`:** add `override` and `klipperTargetC` (assign
    `nullptr` for null).
- [ ] **Step 6: Change the dashboard** (`RemoteDashboard.h`).
  - **Override notice:** while `s.override` is true, the hero shows a notice
    and a button with `id="backKlipper"`, labelled "Back to Klipper". The
    button posts `/api/override/end` through `request()` and applies the
    returned status. The notice reads:
    - "Manual override, Klipper wants N °C";
    - "Manual override, Klipper wants off" when the target is 0;
    - "Manual override, waiting for Klipper's target" when the target is
      `null`.
  - **Mode buttons:** they highlight while `!k || s.override`, not just
    `!k`.
  - **Source chip:** reads "Klipper, overridden" during an override.
- [ ] **Step 7: Run all five gates.**
  Expected: the build succeeds, native is 192/192, and Python is 37, 10
  and 97.
- [ ] **Step 8: Commit** with the message
  `feat: make a manual press a temporary override of the Klipper target`.
  It covers the files above.

Bench, for the owner:
- an override at idle, then a print start takes over;
- an override mid-print, then the print's end turns heat off;
- a Moonraker restart during an override keeps it;
- Back to Klipper;
- a timer expiry during an override returns to Klipper's target.

---

### Task 2: SHA-256, HMAC-SHA256 and PBKDF2 in remote-policy

**Files:**
- Create: `lib/remote-policy/src/AuthCrypto.h`, `lib/remote-policy/src/AuthCrypto.cpp`
- Create: `test/test_auth_crypto/test_main.cpp`

**Interfaces:**
- Produces, used by Tasks 5 and 6:
  ```cpp
  namespace iheaterlink::policy {
  constexpr size_t kSha256Len = 32;
  class Sha256 {
   public:
    Sha256();                                   // ready to update
    void update(const uint8_t *data, size_t len);
    void finish(uint8_t out[kSha256Len]);       // then the object is spent
  };
  void sha256(const uint8_t *data, size_t len, uint8_t out[kSha256Len]);
  void hmacSha256(const uint8_t *key, size_t keyLen, const uint8_t *data,
                  size_t len, uint8_t out[kSha256Len]);
  void pbkdf2Sha256(const uint8_t *pw, size_t pwLen, const uint8_t *salt,
                    size_t saltLen, uint32_t iterations, uint8_t *out,
                    size_t outLen);
  bool equalConstantTime(const uint8_t *a, const uint8_t *b, size_t len);
  void toHex(const uint8_t *in, size_t len, char *out);    // lower case; out holds 2*len+1
  bool fromHex(const char *in, uint8_t *out, size_t len);  // exactly 2*len hex digits, either case
  }
  ```

The device runs SHA-256, for token hashes, and HMAC, for proofs. PBKDF2 runs
on the device only in tests. It sits here so that one natively tested copy
defines what `auth.js` must match. The plan uses this one implementation on
the device too, not mbedTLS, so the device and the tests share code.

- [ ] **Step 1: Write the failing tests.** Compare with
  `toHex` + `TEST_ASSERT_EQUAL_STRING`.
  - `test_sha256_empty`: "" gives
    `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`.
  - `test_sha256_abc`: "abc" gives
    `ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad`.
  - `test_sha256_two_block`:
    "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq" gives
    `248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1`.
  - `test_sha256_million_a`: a million 'a', fed as 1000 `update`s of 1000
    bytes, gives
    `cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0`.
  - `test_sha256_split_updates`: "abc" fed as "a" then "bc" matches
    `test_sha256_abc`.
  - `test_hmac_rfc4231_case1`: the key is twenty 0x0b bytes and the data is
    "Hi There". It gives
    `b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7`.
  - `test_hmac_rfc4231_case2`: the key is "Jefe" and the data is
    "what do ya want for nothing?". It gives
    `5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843`.
  - `test_hmac_rfc4231_case6`: the key is 131 bytes of 0xaa and the data is
    "Test Using Larger Than Block-Size Key - Hash Key First". It gives
    `60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54`.
  - **PBKDF2 tests**, with P="password", S="salt" and dkLen 32:
    - `test_pbkdf2_c1`: c=1 gives
      `120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b`.
    - `test_pbkdf2_c2`: c=2 gives
      `ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43`.
    - `test_pbkdf2_c4096`: c=4096 gives
      `c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a`.
  - `test_pbkdf2_long_output`: P="passwordPASSWORDpassword",
    S="saltSALTsaltSALTsaltSALTsaltSALTsalt", c=4096 and dkLen 40 give
    `348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1c635518c7dac47e9`.
  - `test_equal_constant_time`: equal buffers are true. A one-bit
    difference in the last byte is false. A length of 0 is true.
  - **Hex tests:**
    - `test_hex_round_trip`: `toHex({0x00,0xAB,0xFF})` is "00abff", and
      `fromHex("00ABff")` gives the bytes back.
    - `test_hex_rejects_bad_input`: "0g" fails, "abc" for a length of 2
      fails, and "" for a length of 1 fails.
- [ ] **Step 2: Run them and see them fail.**
  Run: `pio test -e native -f test_auth_crypto`.
  Expected: a build failure, because `AuthCrypto.h` is missing.
- [ ] **Step 3: Implement `AuthCrypto`.**
  - SHA-256 follows FIPS 180-4, with a 64-byte block buffer and a 64-bit bit
    count.
  - HMAC follows RFC 2104. A key longer than 64 bytes is hashed first.
  - PBKDF2 follows RFC 8018 §5.2, with blocks `T_i` for i = 1, 2, …
  - `equalConstantTime` ORs every byte difference together and never
    returns early.
- [ ] **Step 4: Run the tests and see them pass.**
  Run: `pio test -e native -f test_auth_crypto`.
  Expected: 15 tests pass.
- [ ] **Step 5: Run all five gates, then commit** with the message
  `feat(policy): add SHA-256, HMAC and PBKDF2 for the web login`.

---

### Task 3: The page's login script (`auth.js`)

**Files:**
- Create: `src/standalone/AuthScript.h`. It holds
  `static const char kAuthJs[] PROGMEM = R"JS(...)JS";`, the only copy of
  the script.
- Create: `scripts/test_auth_js.py`

**Interfaces:**
- Consumes: the vectors and byte formats from Task 2: lower-case hex,
  16-byte salt and nonce, and 32-byte `V` and proof.
- Produces `globalThis.IHA`, used by Tasks 6, 7 and 9:
  - `sha256(u8) → Uint8Array`
  - `hmac(keyU8, dataU8) → Uint8Array`
  - `pbkdf2(pwU8, saltU8, iterations, len) → Uint8Array`
  - `hex(u8) → string` and `unhex(string) → Uint8Array`
  - `utf8(string) → Uint8Array` (TextEncoder)
  - `login(password) → Promise`. It resolves on success. It rejects with an
    `Error` whose message is the server's `error` text.
  - `setPassword(password) → Promise`, which follows the same rules.
- Consumes: the Task 6 endpoints, used by `login` and `setPassword`:
  - `GET /api/auth/challenge` returns
    `{passwordSet, salt?, iterations?, nonce, newSalt, newIterations}`;
  - `POST /api/auth/login` takes a form with `nonce` and `proof`;
  - `POST /api/auth/password` takes a form with `salt`, `iterations` and
    `verifier`.

- [ ] **Step 1: Write the failing test.** `scripts/test_auth_js.py` holds a
  `unittest.TestCase`.
  - It skips when `shutil.which("node")` is None.
  - It reads `src/standalone/AuthScript.h`, extracts the text between
    `R"JS(` and `)JS"`, and runs it with `node -e` through
    `vm.runInThisContext`.
  - A small harness prints a JSON object of the hex results. The test
    asserts each one equals the Task 2 vectors:
    - SHA-256 of "", "abc", the two-block message and a million 'a';
    - HMAC cases 1, 2 and 6;
    - PBKDF2 c=1, c=2, c=4096 and the 40-byte case.
  - `test_script_is_ascii`: every byte of the script is below 0x80.
- [ ] **Step 2: Run it and see it fail.**
  Run: `python -m unittest scripts.test_auth_js -v`.
  Expected: it fails because `AuthScript.h` is missing. It must not be
  skipped: Node v24 is installed.
- [ ] **Step 3: Write `kAuthJs`.** Write ES2017 with no dependencies.
  - It may not use `crypto.subtle`, which doesn't exist on plain HTTP.
    `TextEncoder` is fine.
  - **`login`:**
    1. GET the challenge.
    2. If `passwordSet` is false, reject with "No password is set yet."
    3. Compute `V = pbkdf2(utf8(pw), unhex(salt), iterations, 32)` and
       `proof = hmac(V, unhex(nonce))`.
    4. POST the hex values with `fetch(..., {method:"POST", body:new URLSearchParams(...)})`.
  - **`setPassword`:** use `newSalt` and `newIterations`, then POST the hex
    `verifier`.
  - Neither function keeps `V` or the password beyond the call.
- [ ] **Step 4: Run it and see it pass.**
  Run: `python -m unittest scripts.test_auth_js -v`.
  Expected: 2 tests pass.
- [ ] **Step 5: Run all five gates.** Expected: scripts is now 99. Then
  commit with the message `feat(web): add the login page's SHA-256, HMAC and PBKDF2`.

---

### Task 4: Request decisions (`AuthPolicy`)

**Files:**
- Create: `lib/remote-policy/src/AuthPolicy.h`, `lib/remote-policy/src/AuthPolicy.cpp`
- Create: `test/test_auth_policy/test_main.cpp`

**Interfaces:**
- Consumes: `fromHex` from Task 2.
- Produces, used by Tasks 6, 7 and 8:
  ```cpp
  namespace iheaterlink::policy {
  enum class RequestClass : uint8_t { Public, LoginEndpoint, ProtectedPage, Action };
  // path excludes the query. Anything that isn't POST is treated as a GET.
  RequestClass classifyRequest(const char *path, bool isPost, bool passwordSet);

  // origin/referer/fwd* are "" when absent. host is the Host header.
  bool originAllowed(const char *origin, const char *referer, const char *host,
                     bool peerTrusted, const char *fwdProto, const char *fwdHost);

  constexpr size_t kMaxTrustedProxies = 4;
  bool parseIpv4(const char *text, uint32_t &out);  // "a.b.c.d" -> a<<24|b<<16|c<<8|d
  void formatIpv4(uint32_t ip, char out[16]);
  bool parseProxyList(const char *csv, uint32_t out[kMaxTrustedProxies], size_t &count);
  bool isTrustedProxy(uint32_t peer, const uint32_t *list, size_t count, uint32_t automatic);
  uint32_t clientAddress(uint32_t peer, bool peerTrusted, const char *xForwardedFor);
  bool viaHttpsProxy(bool peerTrusted, const char *fwdProto);

  bool headerLoginOk(bool enabled, bool peerTrusted, const char *user,
                     const char *allowedUsers, const char *groups,
                     const char *allowedGroups);
  bool validHeaderName(const char *name);

  enum class AuthMethod : uint8_t { None, Password, Header };
  AuthMethod authMethod(bool cookieValid, bool headerOk);  // the cookie wins

  enum class AuthVerdict : uint8_t { Allow, NeedLogin, BadOrigin, ProxyOnly };
  struct RequestFacts {
    RequestClass cls;
    bool apMode;
    bool proxyOnly;
    bool viaHttpsProxy;
    bool originOk;
    bool loggedIn;
  };
  AuthVerdict decideRequest(const RequestFacts &f);

  bool proxyOnlySaveAllowed(bool turningOn, bool viaHttpsProxy);
  bool safeNextPath(const char *next);
  constexpr size_t kSessionTokenLen = 16;
  bool sessionTokenFromCookie(const char *cookieHeader, uint8_t token[kSessionTokenLen]);
  }
  ```

**Rules the tests pin:**
- **`classifyRequest`:**
  - `LoginEndpoint`: `GET /api/auth/challenge`, `POST /api/auth/login`, and
    `POST /api/auth/password` while no password is set.
  - `ProtectedPage`: `GET /setup` and `GET /fw`.
  - `Action`: every other POST, `/api/auth/password` included once a
    password is set.
  - `Public`: every other GET.
- **`originAllowed`:**
  - A non-empty `Origin` is used as given. `"null"` or anything unparseable
    refuses; there is no fall-back to `Referer`.
  - When `Origin` is empty, the scheme, host and port of `Referer` are used.
    Both empty refuses.
  - Only `http` and `https` are accepted. Scheme and host compare
    case-insensitively. A missing port is 80 for http and 443 for https.
  - The candidates are `http://<host>`, and `<fwdProto>://<fwdHost>` when
    `peerTrusted` and both forwarded values are non-empty.
- **`parseProxyList`:** the list is comma-separated with spaces trimmed. An
  empty list is valid with a count of 0. It fails on more than 4 entries, on
  a bad entry, and on 0.0.0.0 or 255.255.255.255.
- **`isTrustedProxy`:** a peer of 0 is never trusted. An `automatic` of 0
  means no automatic entry.
- **`clientAddress`:** when trusted, it is the last comma-separated entry of
  `X-Forwarded-For`, trimmed, if that entry parses. Otherwise it is the
  peer.
- **`headerLoginOk`:**
  - `user` is trimmed and must not be empty.
  - `allowedUsers` and `allowedGroups` are comma lists, compared exactly. An
    empty `allowedUsers` means any user. An empty `allowedGroups` means no
    group check. Otherwise at least one group must be shared.
  - `groups` uses authentik's separator, `|`. The authentik proxy-provider
    docs list `X-authentik-groups` as pipe-separated. Confirm against those
    docs before writing the test, and note the source in a comment.
- **`validHeaderName`:** 1–40 characters from `A–Z a–z 0–9 -`. It is false
  for `Host`, `Cookie`, `Origin`, `Referer` and any name starting with
  `X-Forwarded-`, compared case-insensitively.
- **`decideRequest`:**
  1. `Public` gives Allow.
  2. With `proxyOnly && !apMode && !viaHttpsProxy`, the result is
     ProxyOnly.
  3. `LoginEndpoint` gives Allow, or BadOrigin when the origin fails.
  4. `ProtectedPage` gives Allow when logged in, otherwise NeedLogin.
  5. `Action` gives BadOrigin when the origin fails, then NeedLogin when not
     logged in, then Allow.
- **`proxyOnlySaveAllowed`:** it is `!turningOn || viaHttpsProxy`, where
  "turning on" means off before and on in this save. This stops a direct
  save from locking the owner out; the anti-goal on lockouts asks for that.
- **`safeNextPath`:** starts with `/`, not `//` or `/\`, has no control
  characters and is at most 64 characters.
- **`sessionTokenFromCookie`:**
  - It finds `iheater_session=` at the start or after `;` with optional
    spaces.
  - The value is exactly 32 hex digits, up to `;` or the end.

- [ ] **Step 1: Write the failing tests.** Use `host = "192.168.1.5"`
  unless a test says otherwise.
  - **Request classes:**
    - `test_classify_public_routes`: `/`, `/login`, `/auth.js`,
      `/api/auth/me`, `/generate_204` and `/nope` are Public as GET.
    - `test_classify_login_endpoints`: the challenge GET, the login POST,
      and the password POST with `passwordSet=false`.
    - `test_classify_password_post_once_set_is_action`.
    - `test_classify_protected_pages`: `/setup` and `/fw` as GET.
    - `test_classify_actions`: `/api/mode`, `/api/auth/logout`,
      `/api/override/end`, `/api/ota` and `/nope` as POST.
  - **Origin check:**
    - `test_origin_matches_host`: `("http://192.168.1.5","",host)` is true.
    - `test_origin_default_port_normalised`:
      `("http://192.168.1.5:80","",host)` and
      `("http://192.168.1.5","","192.168.1.5:80")` are true.
    - `test_origin_case_insensitive`:
      `("HTTP://Remote.LAN","","remote.lan")` is true.
    - `test_origin_other_site_refused`: `("http://evil.example","",host)`
      is false.
    - `test_origin_port_mismatch`: `("http://192.168.1.5:8080","",host)` is
      false.
    - `test_origin_scheme_mismatch`: `("https://192.168.1.5","",host)` is
      false.
    - `test_referer_fallback_with_path` (Review Focus 3):
      `("","http://192.168.1.5/setup?x=1",host)` is true.
    - `test_null_origin_refused`: `("null","http://192.168.1.5/",host)` is
      false.
    - `test_both_absent_refused`: `("","",host)` is false.
  - **Forwarded values:**
    - `test_forwarded_from_trusted`:
      `("https://heater.example.com","",host,true,"https","heater.example.com")`
      is true.
    - `test_forwarded_from_untrusted_ignored`: the same with `false` is
      false.
    - `test_forwarded_needs_both_headers`: `(...,true,"https","")` is
      false.
    - `test_forwarded_default_port`:
      `"https://heater.example.com:443"` against forwarded
      `"https"`/`"heater.example.com"` is true.
  - **Proxies and client address:**
    - `test_proxy_list_parse`: `" 192.168.1.10, 10.0.0.2 "` gives a count of
      2 with the right values, and `""` gives a count of 0.
    - `test_proxy_list_rejects`: five entries, `"1.2.3"`, `"256.1.1.1"`,
      `"0.0.0.0"` and `"255.255.255.255"` all fail.
    - `test_trusted_proxy_match`: a listed peer is true and an unlisted peer
      is false. An automatic entry that equals the peer is true. A peer of 0
      is false.
    - `test_client_address_forwarded`: trusted with
      `"10.0.0.9, 192.168.1.20"` gives 192.168.1.20.
    - `test_client_address_untrusted_is_peer`.
    - `test_client_address_bad_forwarded_is_peer`: a last entry of `"x"`.
    - `test_via_https_proxy`: `(true,"https")` and `(true,"HTTPS")` are
      true. `(false,"https")` and `(true,"http")` are false.
  - **Header login:**
    - `test_header_login_off_or_untrusted_refused`.
    - `test_header_login_needs_user`: `""` and `"  "` are false.
    - `test_header_login_any_user`.
    - `test_header_login_allowed_users`: `"alice, bob"` lets "bob" in and
      refuses "carol".
    - `test_header_login_groups`: `"admins|users"` with allowed `"admins"`
      is true. With allowed `"ops"` it is false. Allowed `"admins"` with
      empty groups is false.
    - `test_valid_header_name`: `"X-authentik-username"` is true. `""`, a
      41-character name, `"X Bad"`, `"cookie"` and `"x-forwarded-for"` are
      false.
  - **Login method and verdict:**
    - `test_auth_method_cookie_wins`: `(true,false)` and `(true,true)` give
      Password. `(false,true)` gives Header. `(false,false)` gives None.
    - `test_decide_public_always_allowed`: Allowed even with
      `proxyOnly=true`, not through the proxy, and not logged in.
    - `test_decide_login_endpoint`: a bad origin gives BadOrigin, and a good
      origin gives Allow without a login.
    - `test_decide_protected_page`: NeedLogin, or Allow when logged in.
    - `test_decide_action`: logged in with a bad origin gives BadOrigin. A
      good origin without a login gives NeedLogin. Both give Allow.
    - `test_decide_proxy_only`: a logged-in action sent directly gives
      ProxyOnly. Through the HTTPS proxy it gives Allow. With
      `apMode=true` it gives Allow. A login endpoint and a protected page
      sent directly also give ProxyOnly.
    - `test_proxy_only_save_allowed`: `(true,false)` is false.
      `(true,true)` and `(false,false)` are true.
  - **Redirect path and cookie:**
    - `test_safe_next_path`: `"/setup"` is true. `""`, `"//evil"`,
      `"/\\evil"`, `"http://x"` and a 65-character path are false.
    - `test_cookie_single`: `"iheater_session=<32 hex>"` gives the bytes.
    - `test_cookie_among_others` (Review Focus 4):
      `"a=1; iheater_session=<hex>; b=2"` gives the bytes.
    - `test_cookie_lookalike_name_refused`:
      `"xiheater_session=<hex>"` is false.
    - `test_cookie_bad_value_refused`: 31 hex digits, a non-hex character,
      and the cookie being absent are all false.
- [ ] **Step 2: Run them and see them fail.**
  Run: `pio test -e native -f test_auth_policy`.
  Expected: a build failure, because the header is missing.
- [ ] **Step 3: Implement `AuthPolicy.cpp`** with fixed buffers, no heap and
  no `std::string`. A host is at most 128 characters; anything longer
  refuses.
- [ ] **Step 4: Run the tests and see them pass.**
  Run: `pio test -e native -f test_auth_policy`.
  Expected: all of them pass.
- [ ] **Step 5: Run all five gates, then commit** with the message
  `feat(policy): decide request classes, origin, proxies and header login`.

---

### Task 5: Nonces, lockout, sessions and running hours (`AuthState`)

**Files:**
- Create: `lib/remote-policy/src/AuthState.h`, `lib/remote-policy/src/AuthState.cpp`
- Create: `test/test_auth_state/test_main.cpp`

**Interfaces:**
- Consumes: `sha256`, `hmacSha256` and `equalConstantTime` from Task 2, and
  `intervalElapsed` from `RemotePolicy.h`.
- Produces, used by Tasks 6 and 8:
  ```cpp
  namespace iheaterlink::policy {
  constexpr size_t kNonceLen = 16, kSaltLen = 16, kVerifierLen = 32;
  constexpr uint32_t kMinIterations = 5000, kMaxIterations = 20000, kDefaultIterations = 10000;
  bool validIterations(uint32_t n);
  bool proofMatches(const uint8_t verifier[kVerifierLen], const uint8_t nonce[kNonceLen],
                    const uint8_t proof[kSha256Len]);  // HMAC(V, nonce), constant time

  constexpr size_t kMaxNonces = 4;
  constexpr uint32_t kNonceLifeMs = 60000;
  class NonceTable {
   public:
    void add(const uint8_t nonce[kNonceLen], uint32_t nowMs);  // drops the oldest when full
    bool take(const uint8_t nonce[kNonceLen], uint32_t nowMs); // once, if younger than kNonceLifeMs
    void clear();
  };

  constexpr uint8_t kLockoutFailures = 5;
  constexpr uint32_t kLockoutMs = 30000;
  constexpr size_t kLockoutClients = 8;
  class LoginLockout {
   public:
    uint32_t retryAfterMs(uint32_t client, uint32_t nowMs) const;  // 0: may try now
    void failure(uint32_t client, uint32_t nowMs);
    void success(uint32_t client);
    void clear();
  };

  constexpr size_t kMaxSessions = 4;
  constexpr uint32_t kSessionMaxHours = 720;
  class SessionTable {
   public:
    enum class Check : uint8_t { Valid, Unknown, Expired };
    void add(const uint8_t token[kSessionTokenLen], uint32_t nowHour);  // stores SHA-256(token); evicts the oldest
    Check check(const uint8_t token[kSessionTokenLen], uint32_t nowHour);  // Expired also removes it
    bool remove(const uint8_t token[kSessionTokenLen]);
    void clear();
    size_t count() const;
    static constexpr size_t kBlobLen = kMaxSessions * (1 + kSha256Len + 4);
    void toBlob(uint8_t out[kBlobLen]) const;
    bool fromBlob(const uint8_t *in, size_t len);  // false and empty on a wrong length
  };

  constexpr uint32_t kHourMs = 3600000;
  class RunningHours {
   public:
    void begin(uint32_t storedHours, uint32_t nowMs);
    bool tick(uint32_t nowMs);  // true once per elapsed hour, already counted: save hours()
    uint32_t hours() const;
  };
  }
  ```

**Rules the tests pin:**
- **Nonces:** `take` is wrap-safe. It removes the nonce whether it is fresh
  or expired.
- **Lockout:**
  - The fifth failure in a row locks that client for `kLockoutMs`.
  - Once the lockout ends, the count starts again from 0.
  - `success` clears the client's entry.
  - When the table is full, a new client replaces the entry whose last
    failure is oldest.
- **Sessions:**
  - Slots are kept oldest first, and the blob keeps that order.
  - The age is the unsigned `nowHour - loginHour`, and an age of 720 or more
    has expired. A login hour later than now wraps and counts as expired.
- **Running hours:** a partial hour at a restart is lost. That is accepted,
  and the header comment says so.

- [ ] **Step 1: Write the failing tests.**
  - **Iterations and proof:**
    - `test_valid_iterations`: 4999 is false. 5000, 10000 and 20000 are
      true. 20001 is false.
    - `test_proof_matches`: build V and the nonce, compute
      `hmacSha256(V, nonce)` in the test, and check it is true. Flip one bit
      and it is false.
  - **Nonces:**
    - `test_nonce_single_use`: add at 0, `take` at 1000 is true, a second
      `take` is false.
    - `test_nonce_expires`: add at 0, `take` at 60000 is false.
    - `test_nonce_unknown_refused`.
    - `test_nonce_oldest_dropped`: add five, and the first is refused while
      the fifth is accepted.
    - `test_nonce_wraps`: add at `0xFFFFFF00` and `take` at `0x100` is true.
  - **Lockout:**
    - `test_lockout_after_five`: four failures leave `retryAfterMs` at 0.
      The fifth at t gives 30000 at t. At t+29999 it is 1. At t+30000 it is
      0.
    - `test_lockout_count_restarts`: after the lockout ends, one failure
      gives 0.
    - `test_lockout_success_clears`: four failures, a success, then four
      more failures give 0.
    - `test_lockout_per_client`: client A is locked while client B gives 0.
    - `test_lockout_table_full_drops_oldest`: give nine clients one failure
      each, the first oldest. Four more failures for client 1 leave it
      unlocked, because its entry was dropped and the count restarted.
  - **Sessions:**
    - `test_session_add_check`: Valid. Another token gives Unknown.
    - `test_session_expiry_720h`: login at hour 10. Hour 729 gives Valid.
      Hour 730 gives Expired, then Unknown.
    - `test_session_evicts_oldest`: add five, and the first gives Unknown
      while the second through fifth give Valid.
    - `test_session_remove_and_clear`.
    - `test_session_blob_round_trip`: three sessions survive
      `toBlob`/`fromBlob` in order. A wrong length gives false with a count
      of 0.
    - `test_session_stores_hash_not_token`: the blob doesn't contain the
      16 token bytes.
  - **Running hours:**
    - `test_running_hours_tick`: `begin(10, 0)`. `tick(3599999)` is false.
      `tick(3600000)` is true and `hours()` is 11. `tick(3600001)` is
      false.
    - `test_running_hours_wrap`: `begin(5, 0xFFFFFFFF - 1000)`, then a tick
      at `3600000 - 1001` is true.
    - `test_running_hours_survive_restart`: after the first test,
      `RunningHours b; b.begin(a.hours(), 0);` gives 11, and an hour later
      12.
- [ ] **Step 2: Run them and see them fail.**
  Run: `pio test -e native -f test_auth_state`.
  Expected: a build failure.
- [ ] **Step 3: Implement `AuthState.cpp`** with fixed arrays and no heap.
- [ ] **Step 4: Run the tests and see them pass.**
  Run: `pio test -e native -f test_auth_state`.
  Expected: all of them pass.
- [ ] **Step 5: Run all five gates, then commit** with the message
  `feat(policy): add login nonces, lockout, sessions and running hours`.

---

### Task 6: The request gate and password login in the firmware

**Files:**
- Create: `src/standalone/AuthStore.h`, `src/standalone/AuthStore.cpp`
- Create: `src/standalone/AuthPages.h`. It holds `kLoginHtml`, the
  `/login` page, as a `PROGMEM` raw string.
- Modify: `src/standalone/StandaloneApp.cpp`:
  - `configureRoutes` (:1556);
  - `handleOtaUpload` (:1376) and `handleIheaterImageUpload` (:1437) and
    their final handlers;
  - `setup()` and `loop()`.

**Interfaces:**
- Consumes everything from Tasks 2–5, `kAuthJs` from Task 3, and
  `/api/override/end` from Task 1.
- Produces, used by Tasks 7 and 8:
  ```cpp
  // AuthStore.h. NVS namespace "iheater-auth".
  // Keys: salt, iter, verifier, sessions, hours, proxies, autoProxy, hdrOn,
  // hdrUser, hdrUsers, hdrGrpHdr, hdrGroups, proxyOnly.
  struct AuthSettings {
    bool passwordSet = false;
    uint8_t salt[policy::kSaltLen] = {};
    uint32_t iterations = 0;
    uint8_t verifier[policy::kVerifierLen] = {};
    uint32_t proxies[policy::kMaxTrustedProxies] = {};
    size_t proxyCount = 0;
    bool autoProxy = true;
    bool headerLogin = false;
    String userHeader = "X-authentik-username";
    String allowedUsers;
    String groupsHeader = "X-authentik-groups";
    String allowedGroups;
    bool proxyOnly = false;
  };
  void loadAuth(AuthSettings &out, policy::SessionTable &sessions, uint32_t &hours);
  bool savePassword(const AuthSettings &s);
  bool saveSessions(const policy::SessionTable &t);
  bool saveRunningHours(uint32_t hours);
  bool saveAuthOptions(const AuthSettings &s);  // proxies, autoProxy, header fields, proxyOnly
  // BOOT reset: clears the password, sessions, header login and proxy-only;
  // keeps the proxies and autoProxy.
  bool resetAuth(AuthSettings &s, policy::SessionTable &t);
  ```
- Produces, in `StandaloneApp.cpp`:
  ```cpp
  uint32_t ipValue(const IPAddress &ip);  // ip[0]<<24 | ... ; the policy's byte order
  struct RequestAuth {
    policy::RequestClass cls;
    policy::AuthVerdict verdict;
    policy::AuthMethod method;
    bool viaHttpsProxy;
    uint32_t client;
    String user;
  };
  RequestAuth checkRequest();  // the current s_server request
  void sendRefusal(const RequestAuth &a);
  ```
  It also produces `s_auth`, `s_sessions`, `s_nonces`, `s_lockout` and
  `s_hours`.
- Produces these endpoints, which Task 3's `auth.js` and Task 9's dashboard
  consume:
  - `GET /api/auth/challenge` returns
    `{"passwordSet":true,"salt":"<hex>","iterations":10000,"nonce":"<hex>","newSalt":"<hex>","newIterations":10000}`.
    `salt` and `iterations` are left out when no password is set. Each call
    adds a nonce from `esp_random()`.
  - `POST /api/auth/login` takes `nonce` and `proof` as hex. It replies:
    - 200 `{"ok":true}` with `Set-Cookie`;
    - 429 `{"error":"Too many tries. Wait N s and try again.","retryAfterS":N}`;
    - 401 `{"error":"The login expired. Try again."}` for an unknown or
      expired nonce, which doesn't count as a failure;
    - 401 `{"error":"Wrong password."}`, which counts as a failure;
    - 400 for a malformed request.
  - `POST /api/auth/password` takes `salt`, `iterations` and `verifier` as
    hex. It needs `validIterations`. It saves, ends every session, creates a
    new password session for this request, and replies 200 `{"ok":true}`
    with `Set-Cookie`.
  - `POST /api/auth/logout` removes this request's cookie session if there
    is one, replies 200 `{"ok":true}`, and sets
    `Set-Cookie: iheater_session=; Path=/; Max-Age=0`.
  - `GET /api/auth/me` returns
    `{"loggedIn":bool,"method":"password"|"header"|"none","user":"<header user or empty>","passwordSet":bool,"apOpen":bool}`.
    `apOpen` is `s_apMode` with `IHEATER_AP_PASSWORD` empty.
  - `GET /login` serves `kLoginHtml`; `GET /auth.js` serves `kAuthJs` as
    `application/javascript`.

**Calls this plan makes:**
- After a password is set or changed, every session ends and the person who
  set it gets a new session. The spec says a first set logs the owner in,
  and that a change ends every session; this keeps the changer logged in.
- Headers are read with `s_server.collectAllHeaders()`, which Arduino-ESP32
  3.3.12 has, so the user and group header names can be set in Setup with no
  re-registration.

- [ ] **Step 1: Write `AuthStore`.**
  - Wrong-length blobs load as empty.
  - `passwordSet` means `salt`, `iter` and `verifier` are all present with
    the right lengths and `validIterations(iter)` holds.
  - Proxies are stored as a comma-separated string and parsed with
    `parseProxyList`; a bad value loads as an empty list.
- [ ] **Step 2: Write `checkRequest()`.**
  - Read the peer from `s_server.client().remoteIP()`.
  - Get the trust with `isTrustedProxy(peer, s_auth.proxies, s_auth.proxyCount, 0)`.
    Task 7 replaces the 0 with the Moonraker address.
  - Read `X-Forwarded-Proto`, `X-Forwarded-Host` and `X-Forwarded-For` only
    when the peer is trusted.
  - The cookie is valid when `sessionTokenFromCookie` gives a token and
    `s_sessions.check(token, s_hours.hours())` is `Valid`. If the result is
    `Expired`, call `saveSessions`.
  - Header login is
    `headerLoginOk(s_auth.headerLogin, trusted, header(userHeader), allowedUsers, header(groupsHeader), allowedGroups)`.
  - `loggedIn` is cookie or header. The method comes from `authMethod`.
  - The verdict comes from `decideRequest`.
  - Log a header login on Serial when the user differs from the last one
    logged: `[AUTH] header login user=<name> client=<ip>`.
- [ ] **Step 3: Write `sendRefusal()`.**
  - **NeedLogin on a ProtectedPage:** a 302 to `/login?next=<uri>`.
  - **NeedLogin otherwise:** a 401 with
    `{"error":"Log in first.","login":"/login"}`. With no password set the
    error is "Set a password first."
  - **BadOrigin:** a 403 with
    `{"error":"Refused: the request did not come from this Remote's page."}`.
  - **ProxyOnly:** a 403 with
    `{"error":"This Remote only takes changes through its HTTPS proxy."}`.
    A page gets the same sentence as `text/html`.
  - Log each refusal: `[AUTH] refused <METHOD> <uri>: <login|origin|proxy-only> client=<ip>`.
- [ ] **Step 4: Gate every route in `configureRoutes`.**
  - **Normal routes:** use
    `auto guarded = [](std::function<void()> h) { return [h]() { const RequestAuth a = checkRequest(); if (a.verdict == policy::AuthVerdict::Allow) h(); else sendRefusal(a); }; };`
    on every `s_server.on`, public routes included. Classification is by
    path, so a route added later is protected by default. `onNotFound`
    stays as it is.
  - **Upload routes** (`/api/ota` and `/api/iheater/image`) get a
    different wrapper:
    - At `UPLOAD_FILE_START` it computes `checkRequest()` once and stores it
      in `s_uploadAuth`.
    - It calls the upload function only while `s_uploadAuth.verdict` is
      Allow, for every upload status.
    - The final handler sends `sendRefusal(s_uploadAuth)` without calling
      the handler when the stored verdict isn't Allow. It never recomputes,
      so a stale `s_otaOk` from an earlier upload can't restart the Remote.
  - Review Focus 5: when refused, `handleOtaUpload` is never entered, so
    neither `applyMode(0, "ota:start", false)` nor `Update.begin` runs.
- [ ] **Step 5: Add the endpoints and pages** listed in Interfaces.
  - **The `/login` page:**
    - It loads `/auth.js` and fetches `/api/auth/me`.
    - It shows "Set a password" (password and confirm, at least 8
      characters, checked on the page) when `passwordSet` is false.
      Otherwise it shows "Log in".
    - On success it goes to `next` when `safeNextPath` allows it, otherwise
      to `/`. The page checks `next` as well, with the same rule.
    - Notes:
      - when `location.protocol` isn't `https:`: "This connection isn't
        encrypted. Your password isn't sent, but someone on this network
        could capture enough to log in. Use the HTTPS address through
        nginx, or the Remote's own access point.";
      - when `apOpen`: "This access point is open: anyone nearby can read
        this traffic.";
      - with no password set: "Set a password now: until you do, anyone
        who can reach this Remote can set one."
  - **Serial log lines:**
    - `[AUTH] login ok client=<ip>`
    - `[AUTH] login failed client=<ip> (<n>/5)`
    - `[AUTH] locked out client=<ip> for 30 s`
    - `[AUTH] password set` or `[AUTH] password changed, sessions ended`
    - `[AUTH] logout client=<ip>`
- [ ] **Step 6: Add running hours.**
  - In `setup()`: `loadAuth(...)`, then `s_hours.begin(hours, millis())`.
  - In `loop()`: `if (s_hours.tick(millis())) saveRunningHours(s_hours.hours());`.
  - Call `s_server.collectAllHeaders()` before `s_server.begin()`.
- [ ] **Step 7: Build.** Run the build command. Expected: SUCCESS. Report
  the flash and RAM figures; before this plan they were 70.9% and
  46,816 B.
- [ ] **Step 8: Run all five gates, then commit** with the message
  `feat(web): require a login for actions, with a password challenge and session cookie`.

Bench, for the owner:
- set the first password over the access point;
- log in and out directly;
- a cross-site form POST to `/api/mode` from another origin is refused with
  403;
- a logged-out OTA and a logged-out iHeater image are refused, the Serial
  log shows no `OTA upload start`, and pulse heat keeps running;
- five wrong passwords lock that client out for 30 s;
- a session survives a Remote restart.

---

### Task 7: Security settings, trusted proxies and header login

**Files:**
- Modify: `src/standalone/MoonrakerLink.h`, `src/standalone/MoonrakerLink.cpp`
- Modify: `src/standalone/StandaloneApp.cpp`: `handleSetupPage`, a new
  `handleAuthSettingsPost`, and `checkRequest`.

**Interfaces:**
- Consumes `AuthSettings` and `saveAuthOptions` from Task 6, the policy
  functions from Task 4, and `IHA.setPassword` from Task 3.
- Produces `uint32_t MoonrakerLink::connectedAddress() const`, in the
  policy's byte order, with 0 meaning none.
  - In `onConnected()` it is set from the host. Use
    `IPAddress::fromString` for an address, or else `WiFi.hostByName`,
    which the library's connect has just cached. A change is logged as
    `[MOON] connected address a.b.c.d`.
  - `configure()` clears it only when the host, port or TLS setting differs
    from the previous settings.
- Produces `POST /api/auth/settings`, an Action. Its form fields are:
  - `proxies`;
  - `autoProxy`, `headerLogin` and `proxyOnly` (`1` or missing);
  - `userHeader`, `allowedUsers`, `groupsHeader` and `allowedGroups`.

  It replies 200 `{"ok":true}`, or 400 `{"error":...}` with nothing saved.

- [ ] **Step 1: Add `connectedAddress()`** to `MoonrakerLink`.
- [ ] **Step 2: Set the automatic entry.** In `checkRequest()`, pass
  `(klipperSource() && s_auth.autoProxy) ? s_moonraker.connectedAddress() : 0`
  as `automatic`.
- [ ] **Step 3: Write `handleAuthSettingsPost`.**
  - **Proxies:** `parseProxyList`; a failure gives "Trusted proxies: up to
    four IPv4 addresses, separated by commas."
  - **Header names:** `validHeaderName` on both; a failure gives "Header
    names may use letters, digits and dashes only."
  - **Allowed lists:** at most 128 characters each.
  - **Proxy-only:** check
    `proxyOnlySaveAllowed(new && !old, checkRequest().viaHttpsProxy)`. A
    refusal gives "Turn on proxy-only mode from the HTTPS address, so you
    know it works."
  - **Saving:** call `saveAuthOptions`, then log
    `[AUTH] security settings saved`.
- [ ] **Step 4: Add the "Security" card to `/setup`**, in its own `<form>`
  and outside the main setup form. It loads `/auth.js`.
  - **Change password:** new and confirm fields, at least 8 characters,
    using `IHA.setPassword`. A note says it logs every other device out.
  - **Trusted proxies:** a text field. Above it sits the automatic entry:
    a checkbox, "Trust the Moonraker host automatically", followed by the
    current address, "none yet", or "not used in Manual mode".
    - A note says: "If nginx runs in Docker, add the bridge gateway
      address."
  - **Header login:** an enable checkbox, then the user header, allowed
    users, groups header and allowed groups.
    - A warning says: "Every trusted proxy, the automatic Moonraker entry
      included, can then log anyone in."
  - **Proxy-only:** a checkbox, "Only allow actions through the HTTPS
    proxy".
    - A note says: "Hold BOOT for 10 s to turn this off again. The access
      point always works."
  - **Escaping:** every stored value goes through `htmlEscape` and is
    replaced last, as the existing page does.
  - **Saving:** a JS POST to `/api/auth/settings` with `URLSearchParams`.
    The result shows in the card.
- [ ] **Step 5: Build.** Expected: SUCCESS.
- [ ] **Step 6: Run all five gates, then commit** with the message
  `feat(web): add the Security card, trusted proxies and authentik header login`.

Bench, for the owner:
- the automatic entry shows the Moonraker host's address;
- authentik header login through nginx on the owner's network;
- turning on proxy-only directly is refused, and through nginx it works;
- with proxy-only on, a direct action is refused while the status page and
  WebSocket still work.

---

### Task 8: BOOT reset of the login (10 s hold)

**Files:**
- Modify: `lib/remote-policy/src/RemotePolicy.h`, `lib/remote-policy/src/RemotePolicy.cpp`
- Modify: `test/test_remote_policy/test_main.cpp`
- Modify: `src/standalone/StandaloneApp.cpp`: `updateButton` (:1657) and
  `updateLed` (:1694).

**Interfaces:**
- Consumes `resetAuth`, `s_nonces.clear()` and `s_lockout.clear()` from
  Task 6.
- Produces:
  ```cpp
  constexpr uint32_t kHeatOffHoldMs = 2000;    // today's kLongPressMs moves here
  constexpr uint32_t kAuthResetHoldMs = 10000;
  enum class HoldStep : uint8_t { None, HeatOff, AuthReset };
  // One step per call, in order: heat off first even if a stalled loop sees
  // the hold only at 10 s.
  HoldStep holdStep(uint32_t heldMs, bool heatOffDone, bool resetDone);
  ```

- [ ] **Step 1: Write the failing tests** in `test_remote_policy`.
  - `test_hold_step_heat_off`: `holdStep(1999,false,false)` gives None, and
    `(2000,false,false)` gives HeatOff.
  - `test_hold_step_auth_reset`: `(9999,true,false)` gives None,
    `(10000,true,false)` gives AuthReset, and `(15000,true,true)` gives
    None.
  - `test_hold_step_heat_off_first`: `(12000,false,false)` gives HeatOff.
- [ ] **Step 2: Run them and see them fail.**
  Run: `pio test -e native -f test_remote_policy`.
  Expected: a build failure.
- [ ] **Step 3: Implement `holdStep`.** Then use it in `updateButton`:
  - HeatOff runs today's `applyMode(0, "button:long")`.
  - AuthReset runs:
    - `resetAuth(s_auth, s_sessions)`;
    - `s_nonces.clear()` and `s_lockout.clear()`;
    - `Serial.println("[AUTH] BOOT reset: password, sessions, header login and proxy-only cleared")`;
    - `s_ledSolidUntilMs = millis() + 3000`.
  - The new flag `s_authResetDone` is cleared when a press starts.
  - Releasing after a long hold still does nothing.
  - `updateLed` keeps the LED on while `s_ledSolidUntilMs` is in the
    future, then calls `resetLedPattern()` once.
- [ ] **Step 4: Run the tests and see them pass.**
  Run: `pio test -e native -f test_remote_policy`.
  Expected: they pass.
- [ ] **Step 5: Run all five gates, then commit** with the message
  `feat: clear the web login with a 10 s BOOT hold`.

Bench, for the owner: after a 10 s hold, heat is off from the 2 s mark, the
LED stays on, `/login` offers "Set a password", and Wi-Fi, pairing and the
proxies are unchanged.

---

### Task 9: The dashboard and `/fw` card when logged out, and the WebSocket URL

**Files:**
- Modify: `src/standalone/RemoteDashboard.h`
- Modify: `src/standalone/StandaloneApp.cpp`: `kIheaterCard` (:737–752).

**Interfaces:**
- Consumes `/api/auth/me` and `/api/auth/logout` from Task 6, and the
  override notice from Task 1.

- [ ] **Step 1: Change the dashboard.**
  - **Logged-in check:** on load and after any 401 it fetches
    `/api/auth/me`, then sets `app.dataset.auth` to `"in"` or `"out"`.
  - **Logged out:** CSS hides every control. That covers the mode and quick
    buttons, timers, custom minutes, Wi-Fi save and forget, OTA upload, and
    Back to Klipper.
    - In their place, "Log in to control" links to `/login?next=/`.
  - **Logged in:**
    - by password: a "Log out" button posts `/api/auth/logout`, then
      reloads;
    - by header: the page reads "Logged in as <user>" and shows no Log out.
  - **Errors:** `request()` shows the server's `error` text on a 401 or 403.
- [ ] **Step 2: Fix the status WebSocket URL**, in both the dashboard and
  `kIheaterCard`:
  `location.protocol==="https:" ? "wss://"+location.host+"/ws" : "ws://"+location.hostname+":81/"`.
- [ ] **Step 3: Build.** Expected: SUCCESS.
- [ ] **Step 4: Run all five gates, then commit** with the message
  `feat(web): show controls only when logged in and fix the HTTPS WebSocket URL`.

Bench, for the owner: logged out, the dashboard shows live status and no
controls. Through nginx with HTTPS, status updates arrive over
`wss://<host>/ws`.

---

### Task 10: The optional nginx site in `install.sh`

**Files:**
- Create: `scripts/nginx-site.conf`, the template.
- Modify: `install.sh`: a new `step_nginx_site`, run after
  `restart_services` and before `step_next`.
- Modify: `scripts/test_install.py`: a new `NginxStepTests` class, a
  stubbed `nginx`, and `systemctl reload` logged.

**Interfaces:**
- **Template placeholders:** `SERVER_NAME`, `CERT`, `KEY` and `REMOTE` (the
  Remote's IP address or name).
- **Environment:** `NGINX_DIR` defaults to `/etc/nginx`. The site is
  `${NGINX_DIR}/sites-available/iheater-remote`, linked from
  `${NGINX_DIR}/sites-enabled/iheater-remote`.

**Template content** (README uses it too, in Task 11):
- **Server:** one `server` block with `listen 443 ssl`, `server_name`,
  `ssl_certificate` and `ssl_certificate_key`.
- **Body size:** `client_max_body_size 4m;`. nginx's 1 MB default would
  refuse the roughly 1.4 MB OTA image with a 413.
- **Locations:**
  - `location /` proxies to `http://REMOTE`.
  - `location /ws` proxies to `http://REMOTE:81/`, with
    `proxy_http_version 1.1`, `Upgrade $http_upgrade` and
    `Connection "upgrade"`.
- **Headers on every location.** nginx drops inherited `proxy_set_header`
  lines once a location sets its own, so each location sets the whole list
  itself:
  - `Host $host`;
  - `X-Forwarded-Host $http_host`;
  - `X-Forwarded-Proto $scheme`;
  - `X-Forwarded-For $proxy_add_x_forwarded_for`;
  - `X-authentik-username ""` and `X-authentik-groups ""`.
- **authentik:** a commented `auth_request` block, with the outpost
  location, `auth_request_set $authentik_username` and
  `$authentik_groups`. It notes that the two headers then take those
  variables.
- **Deviation from the spec:** the spec gives `X-Forwarded-Host $host`. This
  plan uses `$http_host`, which keeps a non-default port. The browser's
  `Origin` carries the port, so `$host` would refuse every action on a site
  served on 8443.

- [ ] **Step 1: Write the failing tests.** Add an `nginx` stub. It logs
  `nginx $*` to `STUB_LOG`, and exits 1 for `-t` when `STUB_NGINX_FAIL` is
  set. Also extend the `systemctl` stub to log `reload $2`.
  - `test_nginx_absent_skips_quietly`: there is no stub on `PATH`, the
    output has no nginx prompt, and no files appear under `NGINX_DIR`.
  - `test_nginx_declined`: answering "n" writes nothing.
  - `test_nginx_site_written_and_reloaded`: answer "y", `heater.example.com`,
    the certificate and key paths, and `192.168.1.5`. Then:
    - the site file has all four values substituted and no placeholder
      left;
    - the `sites-enabled` link exists;
    - the log has `nginx -t` then `reload nginx`.
  - `test_nginx_existing_site_backed_up`: a backup of the old file exists,
    and the new file replaces it.
  - `test_nginx_test_failure_rolls_back`: with `STUB_NGINX_FAIL=1` there is
    no reload. The old file is restored, or the new file and link are
    removed when there was none. The output warns, and the install still
    exits 0.
  - `test_nginx_never_touches_other_sites`: a `sites-available/mainsail`
    file is byte-identical afterwards.
- [ ] **Step 2: Run them and see them fail.**
  Run: `python -m unittest discover -s scripts -p "test_*.py"`.
  Expected: the new tests fail.
- [ ] **Step 3: Write `step_nginx_site`** with the `common.sh` helpers
  (`ask`, `ask_value`, `backup`, `ok`, `skip` and `warn`).
  - It skips when `command -v nginx` fails.
  - It asks "Set up an HTTPS nginx site for the Remote?", defaulting to
    no. A blank answer or end of input skips it, so existing tests are
    unchanged.
  - It fills the template, then runs `sudo nginx -t`. On a pass it runs
    `sudo systemctl reload nginx`; on a failure it rolls back and warns.
- [ ] **Step 4: Run the tests and see them pass.**
  Run: `python -m unittest discover -s scripts -p "test_*.py"`.
  Expected: all pass, which is 99 plus the new ones.
- [ ] **Step 5: Run all five gates, then commit** with the message
  `feat(install): offer an HTTPS nginx site for the Remote`.

---

### Task 11: README and AGENTS.md

**Files:**
- Modify: `README.md` (a new "Login and security" section)
- Modify: `AGENTS.md` (Rules)

- [ ] **Step 1: Write the README section "Login and security".** It covers:
  - setting the first password straight away, and why;
  - what plain HTTP exposes;
  - the BOOT 10 s reset and what it keeps;
  - nginx with HTTPS: the `install.sh` step, and the template's server
    block as a snippet;
  - trusted proxies, the automatic Moonraker entry, and the Docker bridge
    note;
  - authentik header login, with its warning;
  - proxy-only mode and its access-point exception;
  - the manual override and "Back to Klipper", under the existing Klipper
    section.
- [ ] **Step 2: Add an AGENTS.md rule:** "Never log or return the login
  verifier, a proof or a session token, and check login before writing any
  upload data."
- [ ] **Step 3: Run all five gates, then commit** with the message
  `docs: explain the web login, nginx and the manual override`.

---

## Bench checks (owner, after the branch is complete)

Each task above lists its own checks. Every upload to a device needs the
owner's go-ahead first. The printer's Remote at 192.168.81.77 is not
touched without asking.
