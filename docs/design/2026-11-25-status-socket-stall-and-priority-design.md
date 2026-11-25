# Status socket: no stall on dead clients, slots kept for logged-in pages

Approved by the owner on 2026-11-24 (short design: a small change).

## Why

- **Stall:** a client that vanishes without closing (a phone that sleeps,
  Wi-Fi that drops) leaves a half-open TCP connection. Once its send buffer
  (5744 bytes) fills, every frame to it blocks loopTask. The core's
  `NetworkClient::write` retries a full buffer 10 times with a 1 s `select`,
  so one frame can block for about 10 s. Frames go to the clients in turn, so
  one dead client delays every page and the whole loop each second, until
  lwIP gives up on the connection (minutes).
- **Slots:** the status socket (port 81, and `/ws` through the nginx proxy)
  has 5 client slots and needs no login. Five logged-out pages, or anyone on
  the network, can take them all and lock a logged-in owner out of live
  status and the UART console.

## Goals

- A dead client costs loopTask at most one short wait, then it is closed.
- A live but briefly slow client keeps its connection, and gets what it
  missed.
- A logged-in page can always connect, even when logged-out pages hold the
  other slots.

Non-goals: more slots, login for the status socket itself (it stays public),
and demoting a socket when its session logs out (checked once, at connect).

## Approaches

1. **A zero-wait check before each frame:** lwIP reports a socket writable
   only when about half its buffer (2873 bytes) is free. It is cheap, but a
   console frame can reach about 3.3 KB, so a frame could still block.
2. **Bound every write (chosen, together with 1):** the library sends every
   byte through the virtual `WebSockets::write`. A subclass replaces it with
   a non-blocking `send` and a fixed time budget, whatever the frame size.
   The zero-wait check stays in front of it, so a status frame to a slow
   client is skipped instead of cut off.
3. **Heartbeat pings only:** the library's heartbeat can detect dead
   clients, but each ping is itself a write that blocks, so it does not
   bound the stall.

For the slots, the session cookie is host-scoped, so the browser sends it
with the socket handshake on port 81 as well. The proxy's user and groups
headers reach `/ws` through nginx. Reading those headers at handshake time
needs no page change. A one-time ticket sent after connecting (as the
console does) was rejected: the socket would have to connect as logged-out
first, and on a full server that is exactly the socket that gets closed.

## Design

- **`StatusSocket`** (`src/standalone/StatusSocket.*`) subclasses
  `WebSocketsServer`:
  - **Bounded write:** `write()` uses `send(..., MSG_DONTWAIT)` with
    `select`, for at most `kSocketWriteBudgetMs` (250 ms) per write. Past the
    budget it stops the TCP connection, and the library then reports the
    client as disconnected. A polite close frame would block as well.
  - **Gated sends:** `sendFrame(num, text)` checks writability with zero
    wait, then asks the per-client `SocketGate`. A frame that finds no room
    is held back (Deferred), never cut; a client with no room for
    `kSocketStuckMs` (3 s) is closed. The caller makes up a held-back frame:
    - **Status:** sent again on the next status pass, even if unchanged.
    - **Backlog frames:** the cursor does not move; the frame is tried again
      on the next loop.
    - **Live `ev` and `hist`:** the client goes back into its backlog from
      the last event and sample it got (`FeedCursor`). The pages keep events
      by seq and samples by time, so a resent frame does no harm.
    - **Console (`uart`):** lines cannot be replayed; the client gets a
      "N lines dropped" note before its next lines.
    - Closing on the first held-back stream frame was rejected: a phone
      saving power acknowledges slowly, and the backlog after an hour of
      uptime (three `hist` frames of about 1.7 KB, events up to 1.6 KB) fills
      half its send buffer. Its page would be closed, reconnect and get the
      same backlog, for good.
  - **Handshake deadline:** a slot still in its handshake after
    `kSocketHandshakeMs` (5 s) is closed. The library has no deadline, and a
    slot in its handshake is never the one closed to make room, so an idle
    connection would otherwise hold it for good.
  - **Handshake headers:** each header line reaches a callback with the
    client number. `clientIsConnected(WSclient_t *)` is overridden to note
    the client that `handleClientData` is about to read.
  - **Deferred closes:** the slot rule's closes run after the library's
    loop, not inside its callbacks.
- **Policy** (`lib/remote-policy/src/SocketPolicy.*` and `FeedCursor.*`,
  native tests):
  - `SocketGate::decide(writable, nowMs)` returns Send, Skip or Close.
  - `HandshakeTimer::expired(inHandshake, nowMs)`.
  - `histWindow` (the next history frame for a cursor) and the resync
    cursors.
  - `slotToClose(slots, n, newcomer)` takes each slot's state: connected,
    logged in, closing, and connect order.
    - It acts only when the newcomer fills the last free slot. Closing slots
      count as free.
    - A logged-out newcomer is closed itself.
    - For a logged-in newcomer, the oldest logged-out socket is closed. If
      every other socket is logged in, none is closed.
    - So logged-out pages hold at most 4 of the 5 slots, and one slot stays
      free for a login.
- **Logged in:** the same rule as HTTP requests: a valid session cookie or
  an accepted proxy header login, from the handshake's headers and peer
  address. With no password and no header login configured, every socket
  counts as logged in, so 5 pages still fit.
- **Pages:** a page closed for room gets close code 4001. The dashboard
  and `/diag` show "Too many open pages: close one or log in" and retry
  after 10 s. The update page retries after 10 s with no message.

## Limits

- Anyone on the network can still race a login for the one free slot, or
  fill all five with handshakes that each last just under 5 s. Deciding at
  accept time would need the library's `handleNewClients` replaced.
- Read stalls predate this change: `WEBSOCKETS_TCP_TIMEOUT` (3 s) also
  bounds each handshake line and each partial frame read, so a peer that
  feeds bytes slowly can still hold loopTask for seconds.
- A console frame (up to about 3.3 KB) can be larger than the room the
  zero-wait check promises (about 2.9 KB); it then waits within the 250 ms
  budget, and a client that cannot take the rest in time is closed.

## Left to the builder

The exact budget, stuck and handshake values may be tuned on the bench.
Check on the bench that the 4001 close reaches Chrome and Safari as 4001,
not 1006: the socket closes right after the close frame, and late data
from the browser can make lwIP answer with a reset. A connection
stopped while lwIP still holds unsent data lingers in lwIP (about 5.7 KB of
buffers) until its retries run out, because `SO_LINGER` is off in this
build; that cost is accepted.
