#pragma once

#include <stddef.h>
#include <stdint.h>

namespace iheaterlink::policy {

// Where a status socket client is in the event log and the temperature
// history while the feed catches it up (its backlog). The pages keep events
// by seq and samples by time, so a frame sent twice does no harm; a frame
// never sent leaves a gap. A client that misses a live frame therefore goes
// back to its backlog from the last thing it got.
struct FeedCursor {
  bool histStage = false;  // events first, then history
  bool evFirstSent = false;
  uint32_t evSeq = 0;  // last event seq sent (0 = none)
  bool histFirstSent = false;
  bool histStarted = false;
  uint32_t histNextMs = 0;  // time of the next sample to send
};

// The next history frame for a cursor wanting samples from `nextMs` on, from
// a ring of `size` samples (size > 0) `stepMs` apart whose newest is at
// `newestMs`: samples [from, from + n), at firstMs .. lastMs. `done`: the
// cursor is past the newest sample (nothing to send). A cursor whose samples
// scrolled out of the ring starts again at the oldest. Wrap-safe.
struct HistWindow {
  bool done;
  size_t from;
  size_t n;
  uint32_t firstMs;
  uint32_t lastMs;
};
HistWindow histWindow(uint32_t nextMs, uint32_t newestMs, size_t size, uint32_t stepMs,
                      size_t maxN);

// A live client missed event `seq` (seq >= 1): it has every event before it
// and every sample in the history (`histSize` samples, newest at
// `histNewestMs`).
FeedCursor cursorAfterMissedEvent(uint32_t seq, size_t histSize, uint32_t histNewestMs,
                                  uint32_t stepMs);

// A live client missed the samples from `fromMs` on: it has every event up
// to `lastEvSeq`.
FeedCursor cursorAfterMissedHist(uint32_t lastEvSeq, uint32_t fromMs);

// A client already in its backlog missed live event `seq`: it sends events
// again (from that one, if the backlog had passed it) before more history.
void rewindForEvent(FeedCursor &cursor, uint32_t seq);

}  // namespace iheaterlink::policy
