#include "FeedCursor.h"

namespace iheaterlink::policy {

HistWindow histWindow(uint32_t nextMs, uint32_t newestMs, size_t size, uint32_t stepMs,
                      size_t maxN) {
  HistWindow w{true, 0, 0, nextMs, nextMs};
  if (size == 0 || stepMs == 0 || maxN == 0) return w;
  if (static_cast<int32_t>(newestMs - nextMs) < 0) return w;  // caught up
  w.done = false;
  const uint32_t back = (newestMs - nextMs) / stepMs;
  if (back < size) {
    w.from = size - 1 - back;
  } else {  // scrolled out of the ring
    w.from = 0;
    w.firstMs = newestMs - static_cast<uint32_t>(size - 1) * stepMs;
  }
  w.n = size - w.from;
  if (w.n > maxN) w.n = maxN;
  w.lastMs = w.firstMs + static_cast<uint32_t>(w.n - 1) * stepMs;
  return w;
}

FeedCursor cursorAfterMissedEvent(uint32_t seq, size_t histSize, uint32_t histNewestMs,
                                  uint32_t stepMs) {
  FeedCursor c;
  c.evFirstSent = true;
  c.evSeq = seq > 0 ? seq - 1 : 0;
  c.histFirstSent = true;
  if (histSize > 0) {
    c.histStarted = true;
    c.histNextMs = histNewestMs + stepMs;
  }
  return c;
}

FeedCursor cursorAfterMissedHist(uint32_t lastEvSeq, uint32_t fromMs) {
  FeedCursor c;
  c.histStage = true;
  c.evFirstSent = true;
  c.evSeq = lastEvSeq;
  c.histFirstSent = true;
  c.histStarted = true;
  c.histNextMs = fromMs;
  return c;
}

void rewindForEvent(FeedCursor &cursor, uint32_t seq) {
  cursor.histStage = false;
  if (seq > 0 && seq <= cursor.evSeq) cursor.evSeq = seq - 1;
}

}  // namespace iheaterlink::policy
