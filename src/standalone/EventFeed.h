#pragma once

#include <Arduino.h>
#include <WebSocketsServer.h>

#include "standalone/StatusSocket.h"

#include <EventLog.h>
#include <TempHistory.h>
#include <UartConsole.h>

namespace iheaterlink {
namespace standalone {

class UartLink;

// The event log and the temperature history, pushed over the status socket
// (port 81) as "ev" and "hist" messages, and the UART console as "uart"
// messages to subscribed clients. Everything runs on loopTask: the web
// handlers and the socket callbacks both run there, so the console tickets
// need no lock.

void feedBegin(StatusSocket &socket);

// The UART link whose exchanges the console shows; nullptr with pulse output
// (a subscription then gets "state":"pulse").
void feedBeginConsole(UartLink *uart);

// A new one-time console ticket (esp_fill_random), kept for redeeming over
// the socket, as 32 hex digits. A secret: send it only in the reply to the
// logged-in request that asked for it; never log it.
void feedIssueTicket(char hexOut[2 * policy::kTicketLen + 1]);

// Formats (vsnprintf into 160 bytes; the ring cuts to kEventTextMax on a UTF-8 boundary), adds to the ring and
// broadcasts the stored entry as a one-event "ev" message. Never pass a
// secret: the text goes to every connected page.
void feedRecord(policy::EventKind kind, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

// Call from the status socket's event handler after the status is sent.
// Handles {"sub":"uart","ticket":"<32 hex>"} and {"sub":""}; a disconnect,
// or a connect that reuses the client number, also unsubscribes. Other text
// is ignored.
void feedOnSocketEvent(uint8_t client, WStype_t type, const uint8_t *payload, size_t len);

// Once per loop: one backlog frame at most, then history sampling (each
// sampling step is broadcast as one "hist" message), then at most one
// "uart" message of console lines to the subscribers.
void feedLoop(uint32_t nowMs, const policy::HistSample &current);

}  // namespace standalone
}  // namespace iheaterlink
