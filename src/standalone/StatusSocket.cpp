#include "standalone/StatusSocket.h"

#include <errno.h>
#include <lwip/sockets.h>

namespace iheaterlink {
namespace standalone {

namespace {

bool waitWritable(int fd, uint32_t waitMs) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(fd, &set);
  timeval tv;
  tv.tv_sec = static_cast<time_t>(waitMs / 1000);
  tv.tv_usec = static_cast<suseconds_t>((waitMs % 1000) * 1000);
  return lwip_select(fd + 1, nullptr, &set, nullptr, &tv) > 0 && FD_ISSET(fd, &set);
}

}  // namespace

void StatusSocket::loop() {
  WebSocketsServer::loop();
  current_ = nullptr;
  for (uint8_t num = 0; num < kMaxClients; num++) {
    const Close how = close_[num];
    if (how == Close::None) continue;
    WSclient_t *client = &_clients[num];
    if (how == Close::NoRoom) {
      // A close frame: code 4001, then the reason. Its write is bounded too.
      static const char kReason[] = "\x0F\xA1" "Too many open pages";
      WebSockets::clientDisconnect(client, kCloseNoRoom, const_cast<char *>(kReason),
                                   sizeof kReason - 1);
    } else {
      // No close frame: the socket has no room for one.
      if (client->tcp) client->tcp->stop();
      clientDisconnect(client);
    }
  }
}

bool StatusSocket::admit(uint8_t num, bool loggedIn) {
  if (num >= kMaxClients) return false;
  admitted_[num] = true;
  loggedIn_[num] = loggedIn;
  order_[num] = ++nextOrder_;
  gates_[num].reset();
  statusMissed_[num] = false;

  policy::SocketSlot slots[kMaxClients];
  for (uint8_t i = 0; i < kMaxClients; i++) {
    const WSclient_t &c = _clients[i];
    slots[i].connected = c.tcp != nullptr && c.status != WSC_NOT_CONNECTED;
    slots[i].loggedIn = loggedIn_[i];
    slots[i].closing = close_[i] != Close::None;
    slots[i].order = order_[i];
    slots[i].pending = !admitted_[i];
  }
  const int victim = policy::slotToClose(slots, kMaxClients, num);
  if (victim != policy::kNoSlot) {
    Serial.printf("[WS] no free slot: closing client %d (logged %s)\n", victim,
                  loggedIn_[victim] ? "in" : "out");
    queueClose(static_cast<uint8_t>(victim), Close::NoRoom);
  }
  return victim != static_cast<int>(num);
}

bool StatusSocket::clientIsOpen(uint8_t num) {
  if (num >= kMaxClients || !admitted_[num] || close_[num] != Close::None) return false;
  return WebSocketsServerCore::clientIsConnected(num) && _clients[num].status == WSC_CONNECTED;
}

bool StatusSocket::anyOpen() {
  for (uint8_t num = 0; num < kMaxClients; num++)
    if (clientIsOpen(num)) return true;
  return false;
}

bool StatusSocket::sendFrame(uint8_t num, const char *text, size_t len,
                             policy::FrameKind kind) {
  if (text == nullptr || !clientIsOpen(num)) return false;
  const bool status = kind == policy::FrameKind::Status;
  switch (gates_[num].decide(kind, writable(num), millis())) {
    case policy::SendAction::Send:
      if (status) statusMissed_[num] = false;
      return sendTXT(num, text, len);
    case policy::SendAction::Skip:
      if (status) statusMissed_[num] = true;
      return false;
    case policy::SendAction::Close:
      Serial.printf("[WS] client %d is not taking data: closing it\n", num);
      queueClose(num, Close::Abort);
      return false;
  }
  return false;
}

void StatusSocket::broadcastFrame(const String &text, policy::FrameKind kind) {
  for (uint8_t num = 0; num < kMaxClients; num++) sendFrame(num, text, kind);
}

void StatusSocket::broadcastStatus(const String &text, bool changed) {
  for (uint8_t num = 0; num < kMaxClients; num++)
    if (changed || statusMissed_[num]) sendFrame(num, text, policy::FrameKind::Status);
}

size_t StatusSocket::write(WSclient_t *client, uint8_t *out, size_t n) {
  if (client == nullptr || out == nullptr || client->tcp == nullptr) return 0;
  if (client->num < kMaxClients && close_[client->num] == Close::Abort) return 0;
  if (!client->tcp->connected()) return 0;
  const int fd = client->tcp->fd();
  if (fd < 0) return 0;

  const uint32_t start = millis();
  size_t total = 0;
  while (total < n) {
    const ssize_t sent = lwip_send(fd, out + total, n - total, MSG_DONTWAIT);
    if (sent > 0) {
      total += static_cast<size_t>(sent);
      continue;
    }
    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
    const uint32_t left = policy::writeBudgetRemaining(start, millis());
    if (left == 0) break;
    waitWritable(fd, left);
  }
  if (total < n && client->num < kMaxClients) {
    // A cut frame leaves the stream unusable: stop it after the loop.
    Serial.printf("[WS] client %d write stalled: closing it\n", client->num);
    queueClose(client->num, Close::Abort);
  }
  return total;
}

bool StatusSocket::clientIsConnected(WSclient_t *client) {
  current_ = client;
  return WebSocketsServerCore::clientIsConnected(client);
}

void StatusSocket::clientDisconnect(WSclient_t *client) {
  if (client->num < kMaxClients) {
    admitted_[client->num] = false;
    loggedIn_[client->num] = false;
    statusMissed_[client->num] = false;
    close_[client->num] = Close::None;
    gates_[client->num].reset();
  }
  if (current_ == client) current_ = nullptr;
  WebSocketsServerCore::clientDisconnect(client);
}

bool StatusSocket::execHttpHeaderValidation(String headerName, String headerValue) {
  if (headerFn_ && current_ != nullptr && current_->status == WSC_HEADER &&
      current_->num < kMaxClients)
    headerFn_(current_->num, headerName, headerValue);
  return true;
}

bool StatusSocket::writable(uint8_t num) {
  WSclient_t &c = _clients[num];
  if (c.tcp == nullptr) return false;
  const int fd = c.tcp->fd();
  return fd >= 0 && waitWritable(fd, 0);
}

void StatusSocket::queueClose(uint8_t num, Close how) {
  if (num >= kMaxClients) return;
  // An abort wins over a polite close: the socket may have no room for one.
  if (close_[num] == Close::None || how == Close::Abort) close_[num] = how;
}

}  // namespace standalone
}  // namespace iheaterlink
