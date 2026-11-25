#include "SocketPolicy.h"

namespace iheaterlink::policy {

SendAction SocketGate::decide(FrameKind kind, bool writable, uint32_t nowMs) {
  if (writable) {
    stuck_ = false;
    return SendAction::Send;
  }
  if (kind == FrameKind::Stream) return SendAction::Close;
  if (!stuck_) {
    stuck_ = true;
    stuckSinceMs_ = nowMs;
  }
  return nowMs - stuckSinceMs_ >= kSocketStuckMs ? SendAction::Close : SendAction::Skip;
}

uint32_t writeBudgetRemaining(uint32_t startMs, uint32_t nowMs) {
  const uint32_t used = nowMs - startMs;
  return used >= kSocketWriteBudgetMs ? 0 : kSocketWriteBudgetMs - used;
}

int slotToClose(const SocketSlot *slots, size_t n, size_t newcomer) {
  if (slots == nullptr || newcomer >= n) return kNoSlot;
  for (size_t i = 0; i < n; i++)
    if (!slots[i].connected || slots[i].closing) return kNoSlot;  // room left
  if (!slots[newcomer].loggedIn) return static_cast<int>(newcomer);
  int oldest = kNoSlot;
  for (size_t i = 0; i < n; i++) {
    if (i == newcomer || slots[i].loggedIn || slots[i].pending) continue;
    if (oldest == kNoSlot ||
        static_cast<int32_t>(slots[i].order - slots[oldest].order) < 0)
      oldest = static_cast<int>(i);
  }
  return oldest;
}

}  // namespace iheaterlink::policy
