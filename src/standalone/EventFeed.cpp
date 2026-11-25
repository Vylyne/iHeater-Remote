#include "standalone/EventFeed.h"

#include <ArduinoJson.h>
#include <AuthCrypto.h>
#include <FeedCursor.h>
#include <esp_random.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "standalone/UartLink.h"

namespace iheaterlink {
namespace standalone {

namespace {

constexpr size_t kEventsPerFrame = 16;
constexpr size_t kSamplesPerFrame = 120;
constexpr size_t kMaxClients = WEBSOCKETS_SERVER_CLIENT_MAX;
// describeExchange cuts by bytes; 192 (its own line limit) never splits a
// UTF-8 character in a full line.
constexpr size_t kConsoleTextBytes = 192;
// A subscribe message is about 55 bytes; anything much longer is not one.
constexpr size_t kMaxSubscribeBytes = 128;

using SendResult = StatusSocket::SendResult;

// Per-client backlog progress. A client also comes back here when it misses
// a live "ev" or "hist" frame (its socket had no room). The console keeps
// its own per-client state, keyed by the same client number.
struct Backlog : policy::FeedCursor {
  uint8_t client = 0;
};

StatusSocket *s_socket = nullptr;
policy::EventLog s_log;
policy::TempHistory s_hist;
Backlog s_queue[kMaxClients];
size_t s_queued = 0;

// The console. s_subscribed is per client number; the ring in UartLink
// exists while any flag is set.
UartLink *s_uart = nullptr;
policy::ConsoleTickets s_tickets;
bool s_subscribed[kMaxClients] = {};
uint32_t s_consoleLost = 0;  // drained lines that could not be sent
// Lines a subscriber missed (its socket had no room); it is told before its
// next lines.
uint32_t s_consoleMissed[kMaxClients] = {};

Backlog *findQueued(uint8_t client) {
  for (size_t i = 0; i < s_queued; i++)
    if (s_queue[i].client == client) return &s_queue[i];
  return nullptr;
}

bool queued(uint8_t client) { return findQueued(client) != nullptr; }

// The head had no room: the others' backlogs go first.
void rotateQueue() {
  if (s_queued < 2) return;
  const Backlog head = s_queue[0];
  for (size_t i = 1; i < s_queued; i++) s_queue[i - 1] = s_queue[i];
  s_queue[s_queued - 1] = head;
}

void enqueue(uint8_t client, const policy::FeedCursor &cursor) {
  if (queued(client) || s_queued >= kMaxClients) return;
  Backlog b;
  static_cast<policy::FeedCursor &>(b) = cursor;
  b.client = client;
  s_queue[s_queued++] = b;
}

uint32_t lastEventSeq() { return s_log.size() > 0 ? s_log.at(s_log.size() - 1).seq : 0; }

// The client missed live event `seq`: it catches up through its backlog.
void resyncEvent(uint8_t client, uint32_t seq) {
  if (Backlog *b = findQueued(client)) {
    policy::rewindForEvent(*b, seq);
    return;
  }
  enqueue(client, policy::cursorAfterMissedEvent(seq, s_hist.size(), s_hist.newestMs(),
                                                 policy::TempHistory::kPeriodMs));
}

// The client (live, not in its backlog) missed the samples from `fromMs` on.
void resyncHist(uint8_t client, uint32_t fromMs) {
  enqueue(client, policy::cursorAfterMissedHist(lastEventSeq(), fromMs));
}

void dropQueued(uint8_t client) {
  for (size_t i = 0; i < s_queued; i++) {
    if (s_queue[i].client != client) continue;
    for (size_t j = i + 1; j < s_queued; j++) s_queue[j - 1] = s_queue[j];
    s_queued--;
    return;
  }
}

bool anySubscribed() {
  for (size_t c = 0; c < kMaxClients; c++)
    if (s_subscribed[c]) return true;
  return false;
}

// True when at least one connected client is not still in its backlog.
bool anyLiveClient() {
  for (uint8_t c = 0; c < kMaxClients; c++)
    if (s_socket->clientIsOpen(c) && !queued(c)) return true;
  return false;
}

// The events in `list` (already in order) as one "ev" frame; empty when
// short of heap. The keys, the kind names and the event texts (the ring
// outlives the serialization below) are stored by pointer, so the document
// needs only its slots.
String eventsFrame(const policy::Event *const *list, size_t n, bool first) {
  String out;
  DynamicJsonDocument doc(JSON_OBJECT_SIZE(4) + JSON_ARRAY_SIZE(n) + n * JSON_ARRAY_SIZE(5));
  if (doc.capacity() == 0) return out;  // out of heap: skip rather than send "null"
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
    row.add(static_cast<const char *>(e.text));
  }
  if (!doc.overflowed()) serializeJson(doc, out);
  return out;
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
  if (!s_socket->clientIsOpen(client)) {
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
    // Not sent (short of heap, no room yet, or the client went: its
    // disconnect may already have moved the queue under `b`): the cursor
    // stays and the frame is tried again on the next loop. A client that is
    // closing is dropped from the queue on the next loop.
    const String out = eventsFrame(list, n, !b.evFirstSent);
    if (out.length() == 0) return;
    const SendResult r = s_socket->sendFrame(client, out);
    if (r != SendResult::Sent) {
      if (r == SendResult::Deferred) rotateQueue();
      return;
    }
    b.evFirstSent = true;
    if (n > 0) b.evSeq = list[n - 1]->seq;
    if (remaining == 0) b.histStage = true;
    return;
  }

  const size_t size = s_hist.size();
  if (size == 0) {
    String out = histFrame(0, 0, 0, true);
    if (s_socket->sendFrame(client, out) == SendResult::Deferred) {
      rotateQueue();
      return;
    }
    dropQueued(client);
    return;
  }
  const uint32_t step = policy::TempHistory::kPeriodMs;
  const uint32_t newest = s_hist.newestMs();
  // Samples keep arriving while the backlog runs, so the cursor is a time,
  // not an index.
  if (!b.histStarted) {
    b.histStarted = true;
    b.histNextMs = newest - static_cast<uint32_t>(size - 1) * step;
  }
  const policy::HistWindow w = policy::histWindow(b.histNextMs, newest, size, step, kSamplesPerFrame);
  if (w.done) {
    dropQueued(client);
    return;
  }
  String out = histFrame(w.from, w.n, w.lastMs, !b.histFirstSent);
  // Not sent: as for events above.
  const SendResult r = s_socket->sendFrame(client, out);
  if (r != SendResult::Sent) {
    if (r == SendResult::Deferred) rotateQueue();
    return;
  }
  b.histFirstSent = true;
  b.histNextMs = w.lastMs + step;
  if (static_cast<int32_t>(newest - b.histNextMs) < 0) dropQueued(client);
}

// {"t":"uart","now":N,"state":"<state>"}; the states are fixed words.
void sendConsoleState(uint8_t client, const char *state) {
  char out[80];
  snprintf(out, sizeof out, "{\"t\":\"uart\",\"now\":%lu,\"state\":\"%s\"}",
           static_cast<unsigned long>(millis()), state);
  s_socket->sendFrame(client, out, strlen(out));
}

// {"t":"uart","now":N,"drop":D,"l":[]}: the lines this client missed.
SendResult sendConsoleDrop(uint8_t client, uint32_t dropped) {
  char out[80];
  snprintf(out, sizeof out, "{\"t\":\"uart\",\"now\":%lu,\"drop\":%lu,\"l\":[]}",
           static_cast<unsigned long>(millis()), static_cast<unsigned long>(dropped));
  return s_socket->sendFrame(client, out, strlen(out));
}

void stopConsole() {
  if (s_uart) s_uart->consoleStop();
  s_consoleLost = 0;
}

void unsubscribe(uint8_t client) {
  if (client >= kMaxClients || !s_subscribed[client]) return;
  s_subscribed[client] = false;
  s_consoleMissed[client] = 0;
  if (!anySubscribed()) stopConsole();
}

// The ticket is checked (and spent) before anything is allocated, so only a
// logged-in page can make the Remote hold the console ring.
void subscribe(uint8_t client, const char *ticketHex) {
  if (client >= kMaxClients) return;
  if (s_uart == nullptr) {
    sendConsoleState(client, "pulse");
    return;
  }
  uint8_t ticket[policy::kTicketLen];
  const bool valid =
      policy::parseTicketHex(ticketHex, ticket) && s_tickets.redeem(ticket, millis());
  memset(ticket, 0, sizeof ticket);
  if (!valid) {
    sendConsoleState(client, "login");
    return;
  }
  if (!s_subscribed[client]) {
    if (!anySubscribed()) {
      s_consoleLost = 0;
      if (!s_uart->consoleStart()) {
        sendConsoleState(client, "memory");
        return;
      }
    }
    s_subscribed[client] = true;
    s_consoleMissed[client] = 0;
  }
  sendConsoleState(client, "on");
}

void onSocketText(uint8_t client, const uint8_t *payload, size_t len) {
  if (payload == nullptr || len == 0 || len > kMaxSubscribeBytes) return;
  StaticJsonDocument<192> doc;
  if (deserializeJson(doc, reinterpret_cast<const char *>(payload), len)) return;
  JsonVariantConst sub = doc["sub"];
  if (!sub.is<const char *>()) return;  // not a subscription message
  const char *name = sub.as<const char *>();
  if (strcmp(name, "uart") == 0)
    subscribe(client, doc["ticket"].as<const char *>());
  else if (name[0] == '\0')
    unsubscribe(client);
}

// How many bytes of a frame the record holds (frames longer than cap are cut).
size_t capturedLen(uint16_t fullLen, size_t cap) { return fullLen < cap ? fullLen : cap; }

// Lowercase hex of the captured part of a frame; out holds 2 * cap + 1.
void captureHex(const uint8_t *bytes, uint16_t fullLen, size_t cap, char *out) {
  policy::toHex(bytes, capturedLen(fullLen, cap), out);
}

// One "uart" frame for `n` drained lines (up to kConsoleDrainMax), empty when
// short of heap:
// {"t":"uart","now":N,"drop":D,"l":[[seq,ms,"text","reqHex","replyHex",reqLen,replyLen],...]}
// The texts and hex strings are copied into a document sized for exactly
// these lines (each text is decoded twice: once to size, once to add).
String consoleFrame(const policy::ConsoleRecord *recs, size_t n, uint32_t dropped) {
  String out;
  char text[kConsoleTextBytes];
  size_t cap = JSON_OBJECT_SIZE(4) + JSON_ARRAY_SIZE(n);
  for (size_t i = 0; i < n; i++) {
    policy::describeExchange(recs[i], text, sizeof text);
    cap += JSON_ARRAY_SIZE(7) + JSON_STRING_SIZE(strlen(text)) +
           JSON_STRING_SIZE(2 * capturedLen(recs[i].reqLen, policy::kConsoleReqBytes)) +
           JSON_STRING_SIZE(2 * capturedLen(recs[i].replyLen, policy::kConsoleReplyBytes));
  }
  DynamicJsonDocument doc(cap);
  if (doc.capacity() == 0) return out;
  doc["t"] = "uart";
  doc["now"] = millis();
  if (dropped > 0) doc["drop"] = dropped;
  JsonArray rows = doc.createNestedArray("l");
  char hex[2 * policy::kConsoleReplyBytes + 1];
  for (size_t i = 0; i < n; i++) {
    const policy::ConsoleRecord &r = recs[i];
    JsonArray row = rows.createNestedArray();
    row.add(r.seq);
    row.add(r.ms);
    policy::describeExchange(r, text, sizeof text);
    row.add(static_cast<char *>(text));  // char *: copied into the document
    captureHex(r.req, r.reqLen, policy::kConsoleReqBytes, hex);
    row.add(static_cast<char *>(hex));
    captureHex(r.reply, r.replyLen, policy::kConsoleReplyBytes, hex);
    row.add(static_cast<char *>(hex));
    row.add(r.reqLen);
    row.add(r.replyLen);
  }
  if (!doc.overflowed()) serializeJson(doc, out);
  return out;
}

// A subscriber whose socket has no room misses the frame; it gets a "drop"
// count for those lines before its next ones.
void sendConsoleLines() {
  bool anyMissed = false;
  for (size_t c = 0; c < kMaxClients; c++)
    if (s_subscribed[c] && s_consoleMissed[c] > 0) anyMissed = true;
  policy::ConsoleRecord recs[UartLink::kConsoleDrainMax];
  uint32_t dropped = 0;
  const size_t n = s_uart->consoleDrain(recs, UartLink::kConsoleDrainMax, dropped);
  dropped += s_consoleLost;
  if (n == 0 && dropped == 0 && !anyMissed) return;
  String out;
  if (n > 0 || dropped > 0) {
    out = consoleFrame(recs, n, dropped);
    if (out.length() == 0) {  // out of heap: count these lines as dropped
      s_consoleLost = dropped + n;
      return;
    }
    s_consoleLost = 0;
  }
  const uint32_t lines = dropped + n;  // what a client that misses `out` misses
  for (uint8_t c = 0; c < kMaxClients; c++) {
    if (!s_subscribed[c] || !s_socket->clientIsOpen(c)) continue;
    if (s_consoleMissed[c] > 0) {
      const SendResult r = sendConsoleDrop(c, s_consoleMissed[c]);
      if (r != SendResult::Sent) {
        if (r == SendResult::Deferred) s_consoleMissed[c] += lines;
        continue;
      }
      s_consoleMissed[c] = 0;
    }
    if (out.length() > 0 && s_socket->sendFrame(c, out) == SendResult::Deferred)
      s_consoleMissed[c] += lines;
  }
}

}  // namespace

void feedBegin(StatusSocket &socket) { s_socket = &socket; }

void feedBeginConsole(UartLink *uart) { s_uart = uart; }

void feedIssueTicket(char hexOut[2 * policy::kTicketLen + 1]) {
  uint8_t ticket[policy::kTicketLen];
  esp_fill_random(ticket, sizeof ticket);
  s_tickets.issue(ticket, millis());
  policy::toHex(ticket, sizeof ticket, hexOut);
  memset(ticket, 0, sizeof ticket);
}

void feedRecord(policy::EventKind kind, const char *fmt, ...) {
  // Wider than the event text: EventLog::add cuts to kEventTextMax on a
  // UTF-8 boundary, which a vsnprintf cut at 64 bytes could split.
  char text[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof text, fmt, args);
  va_end(args);

  const policy::Event &e = s_log.add(kind, text, millis());
  if (s_socket == nullptr || !s_socket->anyOpen()) return;
  const uint32_t seq = e.seq;
  const policy::Event *list[1] = {&e};
  const String out = eventsFrame(list, 1, false);
  if (out.length() == 0) return;
  // A client in its backlog gets it too: its backlog may be past the events.
  for (uint8_t c = 0; c < kMaxClients; c++)
    if (s_socket->clientIsOpen(c) && s_socket->sendFrame(c, out) == SendResult::Deferred)
      resyncEvent(c, seq);
}

void feedOnSocketEvent(uint8_t client, WStype_t type, const uint8_t *payload, size_t len) {
  if (type == WStype_CONNECTED) {
    // A reused client number starts unsubscribed.
    unsubscribe(client);
    if (!queued(client) && s_queued < kMaxClients) {
      Backlog b;
      b.client = client;
      s_queue[s_queued++] = b;
    }
  } else if (type == WStype_DISCONNECTED) {
    dropQueued(client);
    unsubscribe(client);
  } else if (type == WStype_TEXT) {
    onSocketText(client, payload, len);
  }
}

void feedLoop(uint32_t nowMs, const policy::HistSample &current) {
  if (s_socket == nullptr) return;
  if (s_queued > 0) sendBacklogFrame();

  size_t added = s_hist.tick(nowMs, current);
  // Clients still in their backlog get new samples from it, never twice.
  if (added > 0 && anyLiveClient()) {
    // A long stall can add more samples than the ring or a frame holds.
    const size_t size = s_hist.size();
    if (added > size) added = size;
    if (added > kSamplesPerFrame) added = kSamplesPerFrame;
    const uint32_t newest = s_hist.newestMs();
    String out = histFrame(size - added, added, newest, false);
    const uint32_t firstMs =
        newest - static_cast<uint32_t>(added - 1) * policy::TempHistory::kPeriodMs;
    for (uint8_t c = 0; c < kMaxClients; c++)
      if (s_socket->clientIsOpen(c) && !queued(c) &&
          s_socket->sendFrame(c, out) == SendResult::Deferred)
        resyncHist(c, firstMs);
  }

  if (!anySubscribed()) return;
  // A client that went without a disconnect event still lets the ring go.
  for (uint8_t c = 0; c < kMaxClients; c++)
    if (s_subscribed[c] && !s_socket->clientIsOpen(c)) unsubscribe(c);
  if (s_uart && anySubscribed()) sendConsoleLines();
}

}  // namespace standalone
}  // namespace iheaterlink
