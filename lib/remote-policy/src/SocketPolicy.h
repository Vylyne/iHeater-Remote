#pragma once

#include <stddef.h>
#include <stdint.h>

namespace iheaterlink::policy {

// The status socket's send rules (port 81 and /ws). A client that vanished
// without closing keeps a half-open connection whose send buffer fills; the
// rules below keep it from stalling loopTask.

// Longest one write may wait for room before the connection is stopped.
constexpr uint32_t kSocketWriteBudgetMs = 250;
// Longest a client may go without taking a status frame before it is closed.
constexpr uint32_t kSocketStuckMs = 3000;

// Status frames are whole snapshots (the next one replaces a skipped one).
// Stream frames ("ev", "hist", "uart") must all arrive, in order.
enum class FrameKind : uint8_t { Status, Stream };
enum class SendAction : uint8_t { Send, Skip, Close };

// One per client; reset when a client connects.
class SocketGate {
 public:
  void reset() { stuck_ = false; }
  // `writable`: the socket has room now (a zero-wait check).
  SendAction decide(FrameKind kind, bool writable, uint32_t nowMs);

 private:
  bool stuck_ = false;
  uint32_t stuckSinceMs_ = 0;
};

// Milliseconds left of a write's budget; 0 once it is spent. Wrap-safe.
uint32_t writeBudgetRemaining(uint32_t startMs, uint32_t nowMs);

// One client slot. `order` is a connect counter: smaller is older (wrap-safe).
// `closing`: a close is already pending, so the slot counts as free.
// `pending`: still in its handshake; it takes the slot but its login is not
// known yet, so it is never the one closed.
struct SocketSlot {
  bool connected;
  bool loggedIn;
  bool closing;
  uint32_t order;
  bool pending = false;
};

constexpr int kNoSlot = -1;

// Which slot to close after `newcomer` connects, or kNoSlot. Acts only when
// the newcomer took the last free slot: a logged-out newcomer is closed
// itself; a logged-in one closes the oldest logged-out client, so one slot
// stays free for the next login.
int slotToClose(const SocketSlot *slots, size_t n, size_t newcomer);

}  // namespace iheaterlink::policy
