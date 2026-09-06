#pragma once

// A WebSocket server (RFC 6455) on the phi runtime loop - what QWebSocketServer
// was here for, written against Loop watches so the plugin needs no event loop
// of its own (contract 2.0.0). Scope is what this transport uses: text
// messages, ping/pong, clean closes, an origin gate at the handshake and one
// agreed subprotocol. Planned to move into phi-runtime's net:: in 6e-b.

#include <phi/runtime/loop.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <string_view>

namespace phicore::transport::ws {

class WsServer
{
public:
    using ConnId = std::uint64_t;

    struct Callbacks {
        /// The handshake's origin gate. Called with the Origin header verbatim
        /// (empty when the client sent none - not a browser); false refuses
        /// the handshake with 403 before the socket ever speaks WebSocket.
        std::function<bool(const std::string &origin)> acceptOrigin;
        std::function<void(ConnId id, const std::string &peerAddress, std::uint16_t peerPort)> connected;
        std::function<void(ConnId id)> disconnected;
        std::function<void(ConnId id, std::string_view text)> textMessage;
    };

    WsServer() = default;
    ~WsServer();

    WsServer(const WsServer &) = delete;
    WsServer &operator=(const WsServer &) = delete;

    /// Binds and listens. `host` takes an address, "localhost", or the any
    /// aliases ("*", "any", "0.0.0.0", "::", "anyipv6"). One subprotocol is
    /// answered when the client asks for it; browsers that request one refuse
    /// the connection without the echo.
    bool listen(phi::runtime::Loop &loop,
                const std::string &host,
                std::uint16_t port,
                std::string subprotocol,
                Callbacks callbacks,
                std::string *errorString);
    /// Closes every connection and the listener. Safe to call twice.
    void close();

    void sendText(ConnId id, std::string_view text);
    /// Starts a clean close: close frame now, the socket dies when the peer
    /// answers or the write drains, whichever the kernel makes of it.
    void closeConnection(ConnId id, std::uint16_t code, std::string_view reason);

    std::size_t connectionCount() const { return m_conns.size(); }
    /// Every connection that is not already being torn down.
    std::vector<ConnId> connectionIds() const;
    bool isListening() const { return m_listenFd >= 0; }

private:
    struct Conn {
        ConnId id = 0;
        int fd = -1;
        bool open = false;      // handshake done
        bool closed = false;    // torn down, waiting for the deferred reap
        bool sentClose = false;
        std::string peerAddress;
        std::uint16_t peerPort = 0;
        phi::runtime::FdWatch readWatch;
        phi::runtime::FdWatch writeWatch;
        std::string inBuffer;
        std::string outBuffer;
        std::uint8_t fragmentOpcode = 0;
        std::string fragmentBuffer;
    };

    void acceptConnections();
    void onReadable(Conn *conn);
    void onWritable(Conn *conn);
    bool progressHandshake(Conn *conn); // false: connection gone
    void processFrames(Conn *conn);
    void handleControlFrame(Conn *conn, std::uint8_t opcode, std::string_view payload);
    void sendFrame(Conn *conn, std::uint8_t opcode, std::string_view payload);
    void failConnection(Conn *conn, std::uint16_t code, std::string_view reason);
    void flushConn(Conn *conn);
    /// Deferred teardown: may run inside one of the connection's own watch
    /// callbacks, so the watches die on the next loop turn.
    void dropConn(Conn *conn);

    phi::runtime::Loop *m_loop = nullptr;
    int m_listenFd = -1;
    phi::runtime::FdWatch m_listenWatch;
    std::string m_subprotocol;
    Callbacks m_callbacks;
    ConnId m_nextId = 1;
    std::map<ConnId, std::shared_ptr<Conn>> m_conns;
};

} // namespace phicore::transport::ws
