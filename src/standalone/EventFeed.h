#pragma once

#include <Arduino.h>
#include <WebSocketsServer.h>

#include <EventLog.h>
#include <TempHistory.h>

namespace iheaterlink {
namespace standalone {

// The event log and the temperature history, pushed over the status socket
// (port 81) as "ev" and "hist" messages. Everything runs on loopTask.

void feedBegin(WebSocketsServer &socket);

// Formats (vsnprintf into kEventTextMax + 1 bytes), adds to the ring and
// broadcasts the stored entry as a one-event "ev" message. Never pass a
// secret: the text goes to every connected page.
void feedRecord(policy::EventKind kind, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

// Call from the status socket's event handler after the status is sent.
void feedOnSocketEvent(uint8_t client, WStype_t type, const uint8_t *payload, size_t len);

// Once per loop: one backlog frame at most, then history sampling (each
// sampling step is broadcast as one "hist" message).
void feedLoop(uint32_t nowMs, const policy::HistSample &current);

}  // namespace standalone
}  // namespace iheaterlink
