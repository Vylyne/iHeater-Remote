# Event log, temperature graph and UART diag page — design

Status: approved (2026-11-24).

## Why

The owner wants to see what the Remote and the iHeater have been doing
without a USB serial cable. Today every log line goes only to `Serial`, and
the dashboard shows a handful of the 22 input registers the Remote reads
every 200 ms. Two needs, two places:

- **Main page:** a condensed history of what happened (errors, faults,
  actions), plus a temperature graph. It answers "what went on while I was
  away?" without sensor noise.
- **Diag page:** a live UART console of the Remote's Modbus traffic with the
  iHeater, decoded, with the raw bytes on demand, for chasing a link or
  firmware problem.

## Goals

- The main page shows the last events with real clock times and a graph of
  the last hour of temperatures, both live.
- A new `/diag` page shows every UART exchange as it happens: a decoded line,
  and the request and reply bytes behind a toggle. It also shows the latest
  decoded registers.
- The UART poll keeps its 200 ms period and timing; logging never blocks the
  UART task.

## Non-goals

- Keeping logs or history across a restart. Everything lives in RAM; a
  restart clears it and becomes the first event.
- A console of Moonraker traffic. The diag page is the UART link only.
- Downloading or exporting logs.
- Changing what goes to `Serial`. The serial log stays as it is.

## Anti-goals

- Writing logs or history to flash (wear).
- Any secret on the page or the socket: no login verifier, proof, session
  token, API key, Wi-Fi password or pairing token in an event or console
  line.
- Flooding the main page's socket with console traffic, or spending RAM on
  the console when nobody is watching it.

## Constraints

- ESP32-C3, Arduino-ESP32 3.3.12, `WebServer` on port 80 and
  `WebSocketsServer` on port 81 (also reached as `/ws` through the optional
  nginx proxy). The Moonraker TLS client needs its heap; new buffers must
  stay small.
- The Remote has no wall clock (no NTP). It knows only `millis()`.
- Pages are inline HTML in the firmware and must work with no internet: no
  CDN, no chart library.
- Status is public by the owner's earlier decision (web-auth design): the
  dashboard's status and the status WebSocket stay open to anyone who can
  reach the Remote, and carry nothing per-user.
- Pure decisions go in `lib/remote-policy/` with native tests (AGENTS.md).

## Approaches

1. **Push everything over the existing status WebSocket (chosen by the
   owner).** On connect the main page gets the event backlog and the graph
   history; new events and graph points then arrive as they happen. The diag
   page opens the same socket and subscribes to console lines; only
   subscribed clients get them, and the Remote records console lines only
   while at least one client is subscribed. Costs more socket code (per-client
   subscriptions) but matches how the page already updates.
2. Poll new HTTP endpoints (`/api/events?after=N`, `/api/uart?after=N`) every
   second. Simpler, but a second refresh mechanism, the console arrives in
   1 s batches, and the Remote can't tell when nobody is watching.
3. Build the graph in the browser from status pushes while the page is open.
   Smallest, but the graph starts empty every time; ruled out.

## The design

### Socket protocol

- Today each socket message is the status JSON object. That stays exactly as
  it is, so existing pages keep working.
- New messages are JSON objects with a `"t"` field, which the status object
  never has: `"ev"` (one or more events), `"hist"` (graph samples) and
  `"uart"` (console lines). The dashboards' message handlers must ignore any
  object with a `"t"` they don't know, and must not pass it to their status
  handler.
- On connect the Remote sends the status (as now), then one `"ev"` message
  with the whole event backlog and one `"hist"` message with the whole graph
  history.
- A client subscribes to the console by sending `{"sub":"uart"}` and
  unsubscribes with `{"sub":""}`; closing the socket also unsubscribes.
  Incoming text is otherwise ignored, as now.
- Every event and console line carries a sequence number and `ms` (the
  Remote's `millis()` when it happened). Every `"ev"`, `"hist"` and `"uart"`
  message also carries `"now"` (the Remote's `millis()` when it was sent), so
  a page turns any `ms` into a clock time as `Date.now() - (now - ms)`. The
  status object is not changed, so its change-only push keeps working (my
  call: a `millis()` field in the status would make it differ on every
  check).
- Backlogs are sent compactly, as arrays rather than objects per entry, and
  in frames of at most 16 events or 120 graph samples, one client at a time
  (my call: keeps the transient `String` small while TLS holds its heap).

### Event log (main page)

- A ring of the last 64 events in RAM. Each event: sequence number, `ms`, a
  kind, and a short text of at most 64 characters, formatted on the Remote.
- Events (owner's examples: errors, faults, actions; the list is my call):
  - the Remote started (with its reset reason);
  - Wi-Fi joined, lost, AP mode started;
  - Moonraker connected, paired, unpaired (with the reason text, never the
    token), disconnected;
  - iHeater link up, no reply, updating, protocol unsupported;
  - iHeater fault raised (with `faultName`), cleared; a clear attempted and
    its result;
  - the iHeater restarted (its uptime went backwards; with its reset cause);
  - the target changed, with who changed it: Klipper, a manual press, the
    timer, or the end of a manual override;
  - mode change, timer set and timer expired, manual override started and
    ended;
  - iHeater update started and its outcome (the existing log line's outcome
    and detail); Remote OTA started and its result;
  - setup saved.
- Change detection (link, fault, iHeater restart, target source) is a pure
  tracker in `lib/remote-policy/` that compares the previous and current
  state and returns events, with native tests. Direct actions (a web
  request, an OTA) record their event where they happen.
- Repeats are collapsed: if the same kind and text happen again within 60 s
  of the entry's latest occurrence, the existing entry's count goes up and
  its `ms` moves to the latest occurrence, instead of adding a line (shown as
  "×3"). The Remote re-sends the entry under its original sequence number,
  and the page replaces the entry it has with that number (my call).
- The main page shows the events newest first under the graph, with clock
  times from the browser, and fault and error kinds marked.

### Temperature graph (main page)

- One sample every 10 s, 360 samples: the last hour. Each sample holds air,
  heater and target in tenths of a degree (`int16`), and a "no data" value
  when the link has no snapshot. About 2.2 KB.
- Pulse output (no UART) has no readings; the graph is then hidden.
- The page draws it as inline SVG: three lines, a time axis in clock time,
  and the current values in the legend. No library.
- New samples arrive as `"hist"` messages of one sample.

### UART diag page (`/diag`)

- Console lines are recorded only while at least one socket client is
  subscribed. Recording starts empty when the first client subscribes and
  the buffer is freed when the last one leaves.
- A ring of the last 100 lines, fixed-size records with no text: sequence
  number, `ms`, the result (ok, exception with its code, timeout, bad reply
  with the reason), the full lengths of request and reply, and the raw
  request (up to 32 bytes) and reply (up to 64 bytes); longer frames are cut.
  About 110 B a line, 11 KB in all (my call).
- The UART task copies only that record into the ring under the existing
  spinlock; it does no formatting. The main loop drains new records,
  decodes each with a pure function of (request bytes, reply bytes, result)
  in `lib/remote-policy/`, and sends the decoded line plus the bytes to
  subscribed clients. If the ring overflows between drains, the page shows
  "N lines dropped" (my call: keeps the 200 ms poll and the task stack
  untouched).
- The decoder names FC 03, 04, 06 and 10 requests and their register ranges
  ("read inputs 0x00–0x15", "write 0x0100 = 450"), and the link-bootloader
  ones by name: "enter bootloader" (0x0102 ← 0xB007), "erase" and "boot"
  (0x0300 ← 1 or 2), "chunk 12" (FC10 to 0x0310). The reply summary gives
  the key values (for the inputs read: link protocol, air, heater, PWM,
  fault) or the exception code. Anything else is shown as its function code
  and address.
- Heap guard (my call): the ring is allocated when the first client
  subscribes. If the allocation fails, or free heap would drop below 40 KB,
  the subscription is refused and the page says the Remote is short of
  memory. The event ring (~6 KB) and graph (~2.2 KB) are always resident.
- The flash job's exchanges go through the same path, so an update shows in
  the console too.
- The page has: the console (newest at the bottom, auto-scroll unless the
  user scrolled up), a Pause button (the page stops appending; the Remote
  keeps recording), a "Show bytes" toggle, and Clear (clears the view only).
- Above the console, a register table: the latest decoded inputs, settings
  and bootloader info from `UartLinkView`, updated from the status push.
- With pulse output the page says the UART console needs UART output.

### Access

- Owner's decision (2026-11-24): the event log and the graph are public,
  like status. The console needs a login: `/diag` is a guarded page like
  `/setup`, and the socket accepts `{"sub":"uart"}` only from a client whose
  connection carries a valid session, checked the same way the web server
  checks one (through the proxy, the proxy's login rules apply). A refused
  subscription gets a `"uart"` message saying a login is needed. Nothing in
  the console is sensitive today; this is the safer default, and it stops
  strangers on the network from keeping the console's memory in use.
- Event and console text must never include a secret (anti-goals).
- The main page links to `/diag`.

## Tests

- Native: the event tracker (link transitions, fault raised and cleared,
  iHeater restart, target source, repeat collapse), the event ring and the
  console ring (wrap, sequence numbers, drop count), the graph sampler (10 s
  period, "no data"), and the console line decoder for each request kind
  and result.
- Firmware build gate; manual check of both pages on a bench Remote.

## Left to the builder

- Exact event kinds and wording, within the list above.
- How the console decoder names registers.
- The SVG layout and the page styling, following the existing dashboard.
- Where the rings live in the firmware (a new file is fine).
