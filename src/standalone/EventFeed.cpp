#include "standalone/EventFeed.h"

#include <ArduinoJson.h>
#include <stdarg.h>
#include <stdio.h>

namespace iheaterlink {
namespace standalone {

namespace {

constexpr size_t kEventsPerFrame = 16;
constexpr size_t kSamplesPerFrame = 120;
constexpr size_t kMaxClients = WEBSOCKETS_SERVER_CLIENT_MAX;

// Per-client backlog progress. Later features (the console) add their own
// per-client state next to this, keyed by the same client number.
struct Backlog {
  uint8_t client = 0;
  bool histStage = false;  // events first, then history
  bool evFirstSent = false;
  uint32_t evSeq = 0;  // last event seq sent
  bool histFirstSent = false;
  bool histStarted = false;
  uint32_t histNextMs = 0;  // time of the next sample to send
};

WebSocketsServer *s_socket = nullptr;
policy::EventLog s_log;
policy::TempHistory s_hist;
Backlog s_queue[kMaxClients];
size_t s_queued = 0;

bool queued(uint8_t client) {
  for (size_t i = 0; i < s_queued; i++)
    if (s_queue[i].client == client) return true;
  return false;
}

void dropQueued(uint8_t client) {
  for (size_t i = 0; i < s_queued; i++) {
    if (s_queue[i].client != client) continue;
    for (size_t j = i + 1; j < s_queued; j++) s_queue[j - 1] = s_queue[j];
    s_queued--;
    return;
  }
}

// Sends the events in `list` (already in order) as one "ev" frame. A negative
// client broadcasts.
void sendEvents(int client, const policy::Event *const *list, size_t n, bool first) {
  DynamicJsonDocument doc(4096);
  doc["t"] = "ev";
  doc["now"] = millis();
  if (first) doc["first"] = true;
  JsonArray rows = doc.createNestedArray("e");
  for (size_t i = 0; i < n; i++) {
    const policy::Event &e = *list[i];
    JsonArray row = rows.createNestedArray();
    row.add(e.seq);
    row.add(e.ms);
    row.add(policy::eventKindName(e.kind));
    row.add(e.count);
    row.add(static_cast<const char *>(e.text));  // the ring outlives the serialization below
  }
  String out;
  serializeJson(doc, out);
  if (client < 0)
    s_socket->broadcastTXT(out);
  else
    s_socket->sendTXT(static_cast<uint8_t>(client), out);
}

void appendSample(String &out, int16_t v) {
  if (v == policy::kNoReading) {
    out += "null";
  } else {
    char buf[8];
    snprintf(buf, sizeof buf, "%d", static_cast<int>(v));
    out += buf;
  }
}

// A "hist" frame with samples [from, from + n) of the history; `ms` is the
// time of the last of them. Only numbers and null, so it is built directly
// rather than through a large JSON document.
String histFrame(size_t from, size_t n, uint32_t ms, bool first) {
  String out;
  out.reserve(96 + n * 24);
  out += "{\"t\":\"hist\",\"now\":";
  out += static_cast<unsigned long>(millis());
  if (first) out += ",\"first\":true";
  out += ",\"step\":";
  out += static_cast<unsigned long>(policy::TempHistory::kPeriodMs);
  out += ",\"ms\":";
  out += static_cast<unsigned long>(ms);
  out += ",\"s\":[";
  for (size_t i = 0; i < n; i++) {
    const policy::HistSample &s = s_hist.at(from + i);
    if (i) out += ',';
    out += '[';
    appendSample(out, s.airT10);
    out += ',';
    appendSample(out, s.heaterT10);
    out += ',';
    appendSample(out, s.targetT10);
    out += ']';
  }
  out += "]}";
  return out;
}

// One backlog frame for the queue's head.
void sendBacklogFrame() {
  Backlog &b = s_queue[0];
  const uint8_t client = b.client;
  if (!s_socket->clientIsConnected(client)) {
    dropQueued(client);
    return;
  }

  if (!b.histStage) {
    const policy::Event *list[kEventsPerFrame];
    size_t n = 0;
    size_t remaining = 0;
    for (size_t i = 0; i < s_log.size(); i++) {
      const policy::Event &e = s_log.at(i);
      if (e.seq <= b.evSeq) continue;
      if (n < kEventsPerFrame)
        list[n++] = &e;
      else
        remaining++;
    }
    sendEvents(client, list, n, !b.evFirstSent);
    b.evFirstSent = true;
    if (n > 0) b.evSeq = list[n - 1]->seq;
    if (remaining == 0) b.histStage = true;
    return;
  }

  const size_t size = s_hist.size();
  if (size == 0) {
    String out = histFrame(0, 0, 0, true);
    s_socket->sendTXT(client, out);
    dropQueued(client);
    return;
  }
  const uint32_t step = policy::TempHistory::kPeriodMs;
  const uint32_t newest = s_hist.newestMs();
  const uint32_t oldest = newest - static_cast<uint32_t>(size - 1) * step;
  // Samples keep arriving while the backlog runs, so the cursor is a time,
  // not an index.
  if (!b.histStarted) {
    b.histStarted = true;
    b.histNextMs = oldest;
  }
  size_t from = 0;
  const uint32_t back = (newest - b.histNextMs) / step;
  if (back < size) {
    from = size - 1 - back;
  } else {  // the cursor's samples have scrolled out of the ring
    b.histNextMs = oldest;
  }
  size_t n = size - from;
  if (n > kSamplesPerFrame) n = kSamplesPerFrame;
  const uint32_t lastMs = b.histNextMs + static_cast<uint32_t>(n - 1) * step;
  String out = histFrame(from, n, lastMs, !b.histFirstSent);
  s_socket->sendTXT(client, out);
  b.histFirstSent = true;
  b.histNextMs = lastMs + step;
  if (static_cast<int32_t>(newest - b.histNextMs) < 0) dropQueued(client);
}

}  // namespace

void feedBegin(WebSocketsServer &socket) { s_socket = &socket; }

void feedRecord(policy::EventKind kind, const char *fmt, ...) {
  char text[policy::kEventTextMax + 1];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof text, fmt, args);
  va_end(args);

  const policy::Event &e = s_log.add(kind, text, millis());
  if (s_socket == nullptr) return;
  const policy::Event *list[1] = {&e};
  sendEvents(-1, list, 1, false);
}

void feedOnSocketEvent(uint8_t client, WStype_t type, const uint8_t *, size_t) {
  if (type == WStype_CONNECTED) {
    if (!queued(client) && s_queued < kMaxClients) {
      Backlog b;
      b.client = client;
      s_queue[s_queued++] = b;
    }
  } else if (type == WStype_DISCONNECTED) {
    dropQueued(client);
  }
}

void feedLoop(uint32_t nowMs, const policy::HistSample &current) {
  if (s_socket == nullptr) return;
  if (s_queued > 0) sendBacklogFrame();

  size_t added = s_hist.tick(nowMs, current);
  if (added == 0) return;
  // A long stall can add more samples than the ring or a frame holds.
  const size_t size = s_hist.size();
  if (added > size) added = size;
  if (added > kSamplesPerFrame) added = kSamplesPerFrame;
  // Clients still in their backlog get these samples from it, never twice.
  String out = histFrame(size - added, added, s_hist.newestMs(), false);
  for (uint8_t c = 0; c < kMaxClients; c++)
    if (s_socket->clientIsConnected(c) && !queued(c)) s_socket->sendTXT(c, out);
}

}  // namespace standalone
}  // namespace iheaterlink
