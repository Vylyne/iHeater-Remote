# Web UI login and origin protection — design

Status: approved (2026-11-23).

## Why

The Remote's web UI takes every action without a login: mode and timer,
setup and Wi-Fi saves, Remote OTA and, since the link flash work, flashing
the iHeater's own firmware. Anyone on the network can do all of that, and so
can any web page open in the owner's browser (a cross-site POST needs no
login at all today). The owner wants status to stay readable by anyone and
every action locked.

The web UI is plain HTTP, and TLS on the ESP32-C3 is too heavy (about 40 KB
of RAM per connection, plus certificate warnings). So:

- encryption comes from the owner's existing nginx on the printer host;
- direct HTTP still works, but the password itself never crosses the wire;
- the owner already signs in to everything through authentik, and wants the
  Remote to accept authentik's forward-auth headers.

## Goals

- Every action, and the `/setup` and `/fw` pages, need a login. The
  dashboard's status and the status WebSocket stay public.
- A password login whose password is never sent, even over plain HTTP.
- An origin check on every action, so a page on another site can't act
  through the owner's browser.
- Trusted proxies, so the Remote works behind nginx with HTTPS. The
  Moonraker host is trusted automatically.
- Header login from a trusted proxy (authentik forward auth through nginx).
- An optional mode that takes actions only through the HTTPS proxy.
- A physical reset for a forgotten password.
- An optional nginx site from `install.sh`, and a README snippet.

## Non-goals

- TLS on the ESP32.
- API keys or tokens for scripts. Nothing but the browser calls the
  Remote's HTTP API today; Klipper reaches the Remote through the Remote's
  own outbound Moonraker connection, which this doesn't touch.
- An allowed-origins list for other apps' pages calling the Remote (CORS).
  Nothing needs it yet; it can be added later.
- Several users or roles. One password; header login carries a user name
  only for the log and the page.
- Hiding status. Anyone who can reach the Remote can still read it.

## Anti-goals

- Locking the owner out with no way back short of reflashing the Remote.
- Trusting `X-Forwarded-*` or a login header from anything other than a
  trusted proxy's own TCP connection.
- Storing or logging the password, or anything that is the password.
- Writing a byte of an OTA or iHeater image before the request has passed
  the login and origin checks.
- Breaking the first-run access point or the captive-portal pages.

## Constraints

- Arduino-ESP32 3.3.12 `WebServer` on port 80, `WebSocketsServer` on port 81
  (status push only; incoming text is ignored).
- `WebServer` reads only headers named with `collectHeaders()` first.
- `WebServer` calls an upload callback with the file data before the
  route's final handler runs. The checks must run in the upload callback, at
  `UPLOAD_FILE_START`, for `/api/ota` and `/api/iheater/image`.
- Browsers don't provide `crypto.subtle` on plain HTTP (it needs a secure
  context). The login page must carry its own SHA-256.
- The BOOT button is GPIO9, a strapping pin: holding it at power-up starts
  the C3's download mode. It already sets the mode (click) and turns heat
  off (2 s hold).
- The first-run access point is open by default (`IHEATER_AP_PASSWORD` is
  empty).
- Pure decisions go in `lib/remote-policy/` with native tests (AGENTS.md).

## Approaches

1. **The browser's built-in Digest login** (`WebServer::authenticate`). Very
   little code. But it's MD5, uses the browser's pop-up, has no logout, has
   no cookie (so no `SameSite`), and the browser re-sends it on cross-site
   requests. Rejected.
2. **Our own challenge login, then a session cookie (chosen).** A proper
   login page and logout, the same behaviour direct or through nginx, and
   the hashing is testable in `lib/remote-policy`. The cost is a small
   embedded SHA-256 in the page, and more code to get right.
3. **HTTPS on the ESP32.** Rejected for RAM and certificate warnings; nginx
   does it instead.

The owner picked approach 2, plus nginx for HTTPS, plus authentik header
login.

## Requests

Each request is one of:

- **Public:** `GET /`, the captive-portal pages (`/generate_204`, `/gen_204`,
  `/hotspot-detect.html`, `/library/test/success.html`, `/ncsi.txt`,
  `/connecttest.txt`, `/redirect`), `/login`, and the status WebSocket.
  No checks.
- **Login endpoints:** `GET /api/auth/challenge`, `POST /api/auth/login`,
  `POST /api/auth/password` (first password only while none is set). Origin
  check; no session needed.
- **Protected pages:** `GET /setup`, `GET /fw`. A login; without one they
  redirect to `/login?next=<path>`.
- **Actions:** every other `POST`, including `/api/auth/logout` and a
  password change. Origin check and a login. Without one they return 401
  (403 for a failed origin check) as JSON, and change nothing.

Unknown routes (`onNotFound`, which redirects to the portal in access-point
mode) keep today's behaviour and stay public. No `GET` route has side
effects. Live status reaches the dashboard only through the status
WebSocket, which stays public in every mode, proxy-only included: status is
public by design, and cross-site pages reading it is accepted.

## Origin check

**Peer and forwarded values.** The peer is the TCP peer address of the
request. When the peer is a trusted proxy, the Remote also reads
`X-Forwarded-Host`, `X-Forwarded-Proto` and `X-Forwarded-For`; otherwise it
ignores them.

**No Host check (decided).** The Remote is reached by IP, by a router or
hosts-file name, or through nginx, so a fixed list of hosts would lock out
normal use. DNS rebinding (a hostile name pointed at the Remote) is covered
without one: the session cookie belongs to the real name, so a rebound page
has no login, and header login needs a trusted proxy. The one action that
needs no login, setting the first password, has its own guard (see
"First run and upgrade").

**Origin check.** On login endpoints and actions:

- take `Origin`; if it's absent, take the origin of `Referer`; if both are
  absent, refuse;
- the expected origins are `http://<Host>` and, when the peer is a trusted
  proxy and both forwarded headers are present,
  `<X-Forwarded-Proto>://<X-Forwarded-Host>`;
- compare scheme, host and port, case-insensitively for the scheme and host,
  with default ports normalised (80 for http, 443 for https).

## Trusted proxies

- A setting holding up to four IPv4 addresses.
- **Moonraker host (automatic):** while the control source is Klipper, the
  IP of the Remote's most recent successful Moonraker connection is also
  trusted. It's the address of that connection, not a fresh DNS lookup, and
  it stays trusted through reconnects until a connection succeeds to a
  different address or the Moonraker settings change. With TLS on, that
  connection passed a certificate check. A checkbox turns this off; it's on
  by default. In Manual mode nothing is added.
- **Trust covers the whole host (accepted by the owner).** Anything on a
  trusted host can send forwarded headers, and, with header login on, log
  in. On the owner's setup nginx and Moonraker share the printer host, which
  is already trusted to set the heater's target.
- **Peer only.** Trust is decided by the TCP peer address alone. If nginx
  runs in Docker with a bridge network, the peer is the bridge gateway, and
  the README says to add that address.
- The setup page lists the automatic entry, with its current address, above
  the manual ones.

## Password login

**Stored:** salt (16 random bytes), iteration count, and the verifier
`V = PBKDF2-HMAC-SHA256(password, salt, iterations, 32 bytes)`. `V` is
password-equivalent for this Remote. It's stored only in NVS, never logged,
and never sent by the Remote.

**Login:**

1. `GET /api/auth/challenge` returns the salt, the iterations and a nonce
   (16 random bytes, single use, valid for 60 s; at most four outstanding,
   the oldest dropped).
2. The page computes `V` from the typed password and sends
   `POST /api/auth/login` with the nonce and
   `proof = HMAC-SHA256(V, nonce)`.
3. The Remote checks that the nonce is outstanding, burns it, and compares
   the proof in constant time.
4. On success it creates a session and sets the cookie.

**Failures:** after five failed logins in a row from one client, that
client's logins are refused for 30 s, and the page says to wait. The client
is the peer address or, when the peer is a trusted proxy, the last address
in `X-Forwarded-For`. The Remote tracks up to eight clients; when the table
is full, the oldest entry is dropped.

**Setting and changing:**

- The page asks the Remote for a fresh salt, computes `V`, and sends the
  salt, iterations and `V`. Plain HTTP exposes `V`, which lets a listener log
  in to this Remote, but not the password itself.
- The page says so when it isn't on HTTPS, and suggests nginx or the
  access point. On an open access point it says that anyone nearby can read
  it.
- Changing the password needs a login, and ends every session.

**Iterations:** chosen by the builder, between 5,000 and 20,000, so a phone
computes `V` in under about a second. They're stored with the salt so they
can change later.

## Sessions

- A session is 16 random bytes, sent as cookie `iheater_session` with
  `HttpOnly; SameSite=Strict; Path=/; Max-Age=2592000`, plus `Secure` when
  the request came through a trusted proxy with `X-Forwarded-Proto: https`.
- Up to four sessions; a fifth login replaces the oldest.
- Sessions survive a Remote restart: they're stored in NVS, written only on
  login, logout and password change. Only a hash of each token is stored.
- Sessions end on logout, a password change, the BOOT reset, eviction, or
  expiry.
- **Expiry in running time.** The Remote has no wall clock, and NTP would
  mean an outbound internet call. Instead:
  - a running-hours counter lives in NVS, written once an hour (about
    9,000 writes a year, well within NVS wear-levelling);
  - each session records the counter at login;
  - a session older than 720 running hours (30 days) is refused and
    cleared.

  Time with the Remote unplugged doesn't count. The cookie's `Max-Age`
  ends it in the browser after 30 real days as well.

## Header login (authentik)

Settings, all off or empty by default:

- **Enable header login.** The page warns that every trusted proxy, the
  automatic Moonraker entry included, can then log anyone in.
- **User header:** default `X-authentik-username`.
- **Allowed users:** optional, comma-separated. Empty means any user.
- **Groups header:** default `X-authentik-groups`.
- **Allowed groups:** optional, comma-separated. Empty means no group check.
  The builder confirms authentik's separator for the groups header.

A request is logged in by header when all of these hold:

- header login is on;
- the peer is a trusted proxy;
- the user header is present and not empty;
- the user and group checks pass.

It still needs the origin check. It sets no cookie: every request is checked
on its own, and logging out is authentik's job.

**Either method is enough.** A request is logged in if it has a valid
session cookie or passes header login. A failed header check doesn't cancel
a valid cookie. `GET /api/auth/me` reports the cookie first, if both hold.

## Proxy-only mode

The setting **Only allow actions through the HTTPS proxy** is off by default.
When it's on, protected pages, login endpoints and actions are refused
unless the peer is a trusted proxy and `X-Forwarded-Proto` is `https`. The
one exception is access-point mode, where everything works as if the
setting were off. Public pages and the status WebSocket stay open directly.

## First run and upgrade

The password (its salt, iterations and verifier) is kept in NVS and
survives restarts and updates. A Remote has no password only when it's
new, after a settings wipe, after the BOOT reset, or on the first boot of
the update that adds this feature.

When no password is set:

- protected pages and actions lead to `/login`, which shows **Set a
  password** instead of the login form;
- `POST /api/auth/password` works without a login, once, and logs the
  owner in.

**Whoever gets there first sets it (owner's decision).** No token, button
or access-point requirement. The owner is normally on the page when the
Remote comes up, because they just installed it or ran the update. Until
then, anyone who reaches the Remote, or a hostile page that uses DNS
rebinding to reach it, could set it first. The fix is the BOOT reset. The
page and the README say to set a password straight away. Status keeps
working throughout.

## BOOT reset

Holding BOOT for 10 s while the Remote runs does the following:

- clears the password, the sessions, header login and proxy-only mode;
- keeps Wi-Fi, Moonraker, pairing and the trusted proxy list;
- logs it on Serial and flashes the LED in a pattern the builder chooses;
- needs no restart.

The existing 2 s hold still turns heat off first. Releasing after a long
hold does nothing else, as today (no click and no mode change).

## Web UI

- **`/login`:** the login form, or the Set-a-password form, with the
  plain-HTTP and open-access-point notes.
- **Status socket URL:** both dashboards (`RemoteDashboard.h` and the `/fw`
  card) connect to `ws://<host>:81/` over HTTP. Over HTTPS they connect to
  `wss://<host>/ws`, the same origin, which nginx proxies. Today they use
  `wss://<host>:81/`, which nothing serves.
- **Dashboard:** status for everyone. Controls show only when logged in;
  otherwise a "Log in to control" link. A Log out button when logged in by
  password.
- **`/setup`:** a new "Security" card holding:
  - change password;
  - trusted proxies, with the automatic Moonraker entry and its checkbox;
  - header login and its fields;
  - proxy-only mode.
- **`GET /api/auth/me`:** returns whether this request is logged in, the
  method (password or header) and the header user. The status JSON and its
  WebSocket push carry nothing per-user.
- **Serial log:** logins, failures, lockouts, header logins (user name),
  password changes and the BOOT reset. Never the proof, `V` or a token.

## nginx site

- **README:** a snippet for a dedicated `server` block, which keeps
  absolute paths like `/api/...` working. It sets:
  - `proxy_set_header Host $host`;
  - `X-Forwarded-Host $host`;
  - `X-Forwarded-Proto $scheme`;
  - `location /ws`, proxied with WebSocket upgrade to the Remote's port 81;
  - an authentik `auth_request` block, commented out.

  It overwrites the user and group headers on every location, from the
  `auth_request` variables or as empty, so a client can't send its own.
- **`install.sh`:** an optional step that asks for the server name and the
  certificate and key paths. It:
  - writes the site only if no file of that name exists, otherwise backs
    it up first;
  - runs `nginx -t`;
  - reloads only if the test passes;
  - never edits Mainsail's site.

  Tests go in `scripts/` like the rest of the installer.

## Testing

**Native tests (`lib/remote-policy`):**

- SHA-256, HMAC-SHA256 and PBKDF2-HMAC-SHA256 against published vectors
  (FIPS 180-4, RFC 4231, and the PBKDF2-HMAC-SHA256 vectors);
- request classes;
- set-password allowed only while no password is set;
- session expiry in running hours, and the counter surviving a restart;
- the origin check: missing headers, `Referer` fallback, default ports,
  forwarded values from trusted and untrusted peers;
- trusted-proxy matching, including the automatic entry;
- the header-login decision, with users, groups and an untrusted peer;
- proxy-only mode, including the access-point exception;
- nonce use, reuse and expiry;
- the per-client lockout, including the forwarded client address;
- either-method login, where a valid cookie wins over a failed header check;
- session eviction and ending.

**JavaScript:** the page's SHA-256, HMAC and PBKDF2 match the same vectors.
The builder chooses how to run that check.

**Python:** the installer's nginx step.

**Bench:**

- first-run set-password over the access point;
- login and logout, direct and through nginx;
- `Secure` set through nginx only;
- a cross-site form POST refused;
- OTA and an iHeater image refused before any byte is written when logged
  out;
- authentik header login on the owner's network;
- the BOOT reset;
- proxy-only mode.

## Left to the builder

- Where the crypto lives: one SHA-256 in `lib/remote-policy` for both
  native tests and the device, or mbedTLS on the device with the policy
  copy used only for tests. Either way the verification logic is natively
  tested.
- How to get the Moonraker connection's peer address from the WebSocket
  client. Resolving the host just before connecting and recording that
  address is acceptable.
- The exact iteration count, the LED pattern, and the page wording beyond
  the notes above.
- Whether the login crypto is served as one shared script for all pages or
  is inline in `/login` only. Flash is 70.9% used, so either fits.
- The NVS key names, and whether the auth settings get their own namespace.

## Docs

- **README:** a "Login and security" section covering the password, the
  BOOT reset, nginx with HTTPS, authentik and proxy-only mode.
- **AGENTS.md:** a rule to never log or return the verifier, a proof or a
  session token, and to check login before writing upload data.
