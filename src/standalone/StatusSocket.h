#pragma once

#include <Arduino.h>
#include <WebSocketsServer.h>

#include <functional>

#include <SocketPolicy.h>

namespace iheaterlink {
namespace standalone {

// The status socket (port 81; /ws through the nginx proxy). The library's
// writes block loopTask for up to about 10 s each on a half-open client (the
// core retries a full send buffer 10 times with a 1 s select). This subclass
// bounds every write, skips or closes clients that cannot take data, and
// keeps one slot free for a logged-in page. Everything runs on loopTask.
class StatusSocket : public WebSocketsServer {
 public:
  static constexpr size_t kMaxClients = WEBSOCKETS_SERVER_CLIENT_MAX;
  // Close code for a page closed to make room; the pages show a note for it.
  static constexpr uint16_t kCloseNoRoom = 4001;

  // Called for each handshake header the library does not handle itself
  // (Cookie, proxy login headers, ...), with the client it came from.
  using HeaderFn = std::function<void(uint8_t num, const String &name, const String &value)>;

  explicit StatusSocket(uint16_t port) : WebSocketsServer(port) {}

  void onHandshakeHeader(HeaderFn fn) { headerFn_ = std::move(fn); }

  // The library's loop, then the closes queued during it.
  void loop();

  // Call at WStype_CONNECTED. Records the login, applies the slot rule and
  // queues any close it asks for. False when this client is the one being
  // closed: send it nothing.
  bool admit(uint8_t num, bool loggedIn);

  // Sends through the client's SocketGate: a status frame is skipped while
  // the socket has no room; a stream frame that cannot go closes the client.
  // False when nothing was sent.
  bool sendFrame(uint8_t num, const char *text, size_t len, policy::FrameKind kind);
  bool sendFrame(uint8_t num, const String &text, policy::FrameKind kind) {
    return sendFrame(num, text.c_str(), text.length(), kind);
  }
  // sendFrame to every admitted client.
  void broadcastFrame(const String &text, policy::FrameKind kind);
  // The status to every client when it changed, else only to clients whose
  // last status frame was skipped (they would otherwise stay stale).
  void broadcastStatus(const String &text, bool changed);

  // Admitted, in the WebSocket phase and not closing.
  bool clientIsOpen(uint8_t num);
  // Any client clientIsOpen.
  bool anyOpen();

 protected:
  size_t write(WSclient_t *client, uint8_t *out, size_t n) override;
  bool clientIsConnected(WSclient_t *client) override;
  void clientDisconnect(WSclient_t *client) override;
  bool execHttpHeaderValidation(String headerName, String headerValue) override;

 private:
  enum class Close : uint8_t { None, Abort, NoRoom };

  bool writable(uint8_t num);
  void queueClose(uint8_t num, Close how);

  HeaderFn headerFn_;
  // The client handleClientData is reading: it calls clientIsConnected for
  // a client just before handing that client's header line to handleHeader,
  // which calls execHttpHeaderValidation.
  WSclient_t *current_ = nullptr;
  policy::SocketGate gates_[kMaxClients];
  bool admitted_[kMaxClients] = {};
  bool loggedIn_[kMaxClients] = {};
  bool statusMissed_[kMaxClients] = {};
  Close close_[kMaxClients] = {};
  uint32_t order_[kMaxClients] = {};
  uint32_t nextOrder_ = 0;
};

}  // namespace standalone
}  // namespace iheaterlink
