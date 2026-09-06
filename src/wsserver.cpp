#include "wsserver.h"

#include <openssl/sha.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>

namespace phicore::transport::ws {

namespace {

constexpr std::size_t kMaxHandshakeBytes = 16 * 1024;
constexpr std::size_t kMaxMessageBytes = 16 * 1024 * 1024;

constexpr std::uint8_t kOpContinuation = 0x0;
constexpr std::uint8_t kOpText = 0x1;
constexpr std::uint8_t kOpBinary = 0x2;
constexpr std::uint8_t kOpClose = 0x8;
constexpr std::uint8_t kOpPing = 0x9;
constexpr std::uint8_t kOpPong = 0xA;

std::string toLower(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string_view trimmed(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

/// True when a comma-separated header value names `token` (case-insensitive) -
/// the shape of Connection: keep-alive, Upgrade.
bool listContainsToken(std::string_view value, std::string_view token)
{
    const std::string lowered = toLower(value);
    const std::string wanted = toLower(token);
    std::size_t start = 0;
    while (start <= lowered.size()) {
        std::size_t comma = lowered.find(',', start);
        if (comma == std::string::npos)
            comma = lowered.size();
        if (trimmed(std::string_view(lowered).substr(start, comma - start)) == wanted)
            return true;
        start = comma + 1;
    }
    return false;
}

std::string base64Encode(const unsigned char *data, std::size_t length)
{
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((length + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 3 <= length; i += 3) {
        const std::uint32_t chunk = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += kAlphabet[(chunk >> 18) & 0x3F];
        out += kAlphabet[(chunk >> 12) & 0x3F];
        out += kAlphabet[(chunk >> 6) & 0x3F];
        out += kAlphabet[chunk & 0x3F];
    }
    if (i + 1 == length) {
        const std::uint32_t chunk = data[i] << 16;
        out += kAlphabet[(chunk >> 18) & 0x3F];
        out += kAlphabet[(chunk >> 12) & 0x3F];
        out += "==";
    } else if (i + 2 == length) {
        const std::uint32_t chunk = (data[i] << 16) | (data[i + 1] << 8);
        out += kAlphabet[(chunk >> 18) & 0x3F];
        out += kAlphabet[(chunk >> 12) & 0x3F];
        out += kAlphabet[(chunk >> 6) & 0x3F];
        out += '=';
    }
    return out;
}

std::string acceptKeyFor(std::string_view clientKey)
{
    // The fixed GUID is the protocol's (RFC 6455 §1.3).
    std::string material(clientKey);
    material += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char digest[SHA_DIGEST_LENGTH];
    ::SHA1(reinterpret_cast<const unsigned char *>(material.data()), material.size(), digest);
    return base64Encode(digest, sizeof(digest));
}

} // namespace

WsServer::~WsServer()
{
    close();
}

bool WsServer::listen(phi::runtime::Loop &loop,
                      const std::string &host,
                      std::uint16_t port,
                      std::string subprotocol,
                      Callbacks callbacks,
                      std::string *errorString)
{
    const auto fail = [&](std::string message) {
        if (errorString)
            *errorString = std::move(message);
        return false;
    };
    if (m_listenFd >= 0)
        return fail("Already listening");

    // The address forms the config may speak: the any aliases, localhost, or
    // a literal address - IPv4 or IPv6.
    const std::string normalized = toLower(std::string(trimmed(host)));
    sockaddr_storage addr{};
    socklen_t addrLen = 0;
    int family = AF_INET;
    if (normalized == "*" || normalized == "any" || normalized == "0.0.0.0") {
        auto *a4 = reinterpret_cast<sockaddr_in *>(&addr);
        a4->sin_family = AF_INET;
        a4->sin_addr.s_addr = htonl(INADDR_ANY);
        a4->sin_port = htons(port);
        addrLen = sizeof(sockaddr_in);
    } else if (normalized == "::" || normalized == "anyipv6") {
        auto *a6 = reinterpret_cast<sockaddr_in6 *>(&addr);
        a6->sin6_family = AF_INET6;
        a6->sin6_addr = in6addr_any;
        a6->sin6_port = htons(port);
        family = AF_INET6;
        addrLen = sizeof(sockaddr_in6);
    } else if (normalized == "localhost") {
        auto *a4 = reinterpret_cast<sockaddr_in *>(&addr);
        a4->sin_family = AF_INET;
        a4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a4->sin_port = htons(port);
        addrLen = sizeof(sockaddr_in);
    } else {
        auto *a4 = reinterpret_cast<sockaddr_in *>(&addr);
        auto *a6 = reinterpret_cast<sockaddr_in6 *>(&addr);
        if (::inet_pton(AF_INET, normalized.c_str(), &a4->sin_addr) == 1) {
            a4->sin_family = AF_INET;
            a4->sin_port = htons(port);
            addrLen = sizeof(sockaddr_in);
        } else if (::inet_pton(AF_INET6, normalized.c_str(), &a6->sin6_addr) == 1) {
            a6->sin6_family = AF_INET6;
            a6->sin6_port = htons(port);
            family = AF_INET6;
            addrLen = sizeof(sockaddr_in6);
        } else {
            return fail("Invalid host address: " + host);
        }
    }

    const int fd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return fail(std::string("socket() failed: ") + std::strerror(errno));
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (::bind(fd, reinterpret_cast<const sockaddr *>(&addr), addrLen) < 0) {
        const int err = errno;
        ::close(fd);
        return fail(std::string("Failed to listen on requested host/port: ") + std::strerror(err));
    }
    if (::listen(fd, 32) < 0) {
        const int err = errno;
        ::close(fd);
        return fail(std::string("listen() failed: ") + std::strerror(err));
    }

    m_loop = &loop;
    m_listenFd = fd;
    m_subprotocol = std::move(subprotocol);
    m_callbacks = std::move(callbacks);
    m_listenWatch = m_loop->watchFd(m_listenFd, phi::runtime::Loop::FdEvent::Read,
                                    [this]() { acceptConnections(); });
    return true;
}

void WsServer::close()
{
    // Contract calls run on the loop's thread but never inside one of these
    // watches' own callbacks, so the teardown is direct.
    for (auto &entry : m_conns) {
        Conn *conn = entry.second.get();
        conn->closed = true;
        conn->readWatch.reset();
        conn->writeWatch.reset();
        ::close(conn->fd);
    }
    m_conns.clear();
    m_listenWatch.reset();
    if (m_listenFd >= 0) {
        ::close(m_listenFd);
        m_listenFd = -1;
    }
}

void WsServer::acceptConnections()
{
    while (true) {
        sockaddr_storage peer{};
        socklen_t peerLen = sizeof(peer);
        const int fd = ::accept4(m_listenFd, reinterpret_cast<sockaddr *>(&peer), &peerLen,
                                 SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        const int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        auto conn = std::make_shared<Conn>();
        conn->id = m_nextId++;
        conn->fd = fd;
        char text[INET6_ADDRSTRLEN] = {};
        if (peer.ss_family == AF_INET) {
            const auto *a4 = reinterpret_cast<const sockaddr_in *>(&peer);
            ::inet_ntop(AF_INET, &a4->sin_addr, text, sizeof(text));
            conn->peerPort = ntohs(a4->sin_port);
        } else if (peer.ss_family == AF_INET6) {
            const auto *a6 = reinterpret_cast<const sockaddr_in6 *>(&peer);
            ::inet_ntop(AF_INET6, &a6->sin6_addr, text, sizeof(text));
            conn->peerPort = ntohs(a6->sin6_port);
        }
        conn->peerAddress = text;

        Conn *raw = conn.get();
        conn->readWatch = m_loop->watchFd(fd, phi::runtime::Loop::FdEvent::Read,
                                          [this, raw]() { onReadable(raw); });
        conn->writeWatch = m_loop->watchFd(fd, phi::runtime::Loop::FdEvent::Write,
                                           [this, raw]() { onWritable(raw); });
        conn->writeWatch.setEnabled(false);
        m_conns.emplace(raw->id, std::move(conn));
    }
}

void WsServer::onReadable(Conn *conn)
{
    if (conn->closed)
        return;

    char chunk[32 * 1024];
    while (true) {
        const ssize_t got = ::read(conn->fd, chunk, sizeof(chunk));
        if (got > 0) {
            conn->inBuffer.append(chunk, static_cast<std::size_t>(got));
            continue;
        }
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (got < 0 && errno == EINTR)
            continue;
        dropConn(conn); // peer closed, or a read error
        return;
    }

    if (!conn->open) {
        if (!progressHandshake(conn))
            return;
        if (!conn->open)
            return; // request still incomplete
    }
    processFrames(conn);
}

bool WsServer::progressHandshake(Conn *conn)
{
    const std::size_t end = conn->inBuffer.find("\r\n\r\n");
    if (end == std::string::npos) {
        if (conn->inBuffer.size() > kMaxHandshakeBytes)
            dropConn(conn);
        return conn->inBuffer.size() <= kMaxHandshakeBytes;
    }

    const std::string_view request(conn->inBuffer.data(), end + 2);

    // Request line: a WebSocket handshake is a GET.
    const std::size_t lineEnd = request.find("\r\n");
    const std::string_view requestLine = request.substr(0, lineEnd);
    if (requestLine.substr(0, 4) != "GET ") {
        conn->outBuffer += "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
        flushConn(conn);
        dropConn(conn);
        return false;
    }

    // Headers, keys lowered; values verbatim minus edges.
    std::map<std::string, std::string> headers;
    std::size_t pos = lineEnd + 2;
    while (pos < request.size()) {
        const std::size_t eol = request.find("\r\n", pos);
        const std::string_view line = request.substr(pos, eol - pos);
        pos = eol + 2;
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos)
            continue;
        headers[toLower(line.substr(0, colon))] = std::string(trimmed(line.substr(colon + 1)));
    }

    const auto header = [&headers](const char *name) -> std::string {
        const auto it = headers.find(name);
        return it == headers.end() ? std::string() : it->second;
    };

    const std::string key = header("sec-websocket-key");
    if (!listContainsToken(header("connection"), "upgrade")
        || toLower(header("upgrade")) != "websocket"
        || key.empty()
        || header("sec-websocket-version") != "13") {
        conn->outBuffer += "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
        flushConn(conn);
        dropConn(conn);
        return false;
    }

    // The origin gate: same-origin policy does not cover WebSocket handshakes,
    // so the server checks the header itself; the policy is the transport's.
    const std::string origin = header("origin");
    if (m_callbacks.acceptOrigin && !m_callbacks.acceptOrigin(origin)) {
        conn->outBuffer += "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n";
        flushConn(conn);
        dropConn(conn);
        return false;
    }

    // Subprotocol: answered when asked for. A browser that requested one and
    // gets no echo drops the connection itself.
    const bool wantsSubprotocol = !m_subprotocol.empty()
        && listContainsToken(header("sec-websocket-protocol"), m_subprotocol);

    std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: " + acceptKeyFor(key) + "\r\n";
    if (wantsSubprotocol)
        response += "Sec-WebSocket-Protocol: " + m_subprotocol + "\r\n";
    response += "\r\n";

    conn->inBuffer.erase(0, end + 4);
    conn->outBuffer += response;
    conn->open = true;
    flushConn(conn);
    if (conn->closed)
        return false;
    if (m_callbacks.connected)
        m_callbacks.connected(conn->id, conn->peerAddress, conn->peerPort);
    return !conn->closed;
}

void WsServer::processFrames(Conn *conn)
{
    while (!conn->closed) {
        const auto &buf = conn->inBuffer;
        if (buf.size() < 2)
            return;
        const std::uint8_t b0 = static_cast<std::uint8_t>(buf[0]);
        const std::uint8_t b1 = static_cast<std::uint8_t>(buf[1]);
        const bool fin = (b0 & 0x80) != 0;
        const std::uint8_t opcode = b0 & 0x0F;
        const bool masked = (b1 & 0x80) != 0;
        std::uint64_t length = b1 & 0x7F;
        std::size_t headerLen = 2;

        if ((b0 & 0x70) != 0)
            return failConnection(conn, 1002, "reserved bits set");
        if (!masked)
            return failConnection(conn, 1002, "client frames must be masked");

        if (length == 126) {
            if (buf.size() < 4)
                return;
            length = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(buf[2])) << 8)
                   | static_cast<std::uint8_t>(buf[3]);
            headerLen = 4;
        } else if (length == 127) {
            if (buf.size() < 10)
                return;
            length = 0;
            for (int i = 0; i < 8; ++i)
                length = (length << 8) | static_cast<std::uint8_t>(buf[2 + i]);
            headerLen = 10;
        }
        if (length > kMaxMessageBytes)
            return failConnection(conn, 1009, "message too big");

        const std::size_t frameLen = headerLen + 4 + static_cast<std::size_t>(length);
        if (buf.size() < frameLen)
            return;

        std::uint8_t mask[4];
        std::memcpy(mask, buf.data() + headerLen, 4);
        std::string payload(buf.data() + headerLen + 4, static_cast<std::size_t>(length));
        for (std::size_t i = 0; i < payload.size(); ++i)
            payload[i] = static_cast<char>(payload[i] ^ mask[i & 3]);
        conn->inBuffer.erase(0, frameLen);

        if (opcode >= kOpClose) {
            if (!fin || length > 125)
                return failConnection(conn, 1002, "malformed control frame");
            handleControlFrame(conn, opcode, payload);
            continue;
        }

        if (opcode == kOpBinary)
            return failConnection(conn, 1003, "binary frames are not supported");
        if (opcode == kOpText) {
            if (!conn->fragmentBuffer.empty() || conn->fragmentOpcode != 0)
                return failConnection(conn, 1002, "new message inside a fragmented one");
            if (fin) {
                if (m_callbacks.textMessage)
                    m_callbacks.textMessage(conn->id, payload);
                continue;
            }
            conn->fragmentOpcode = kOpText;
            conn->fragmentBuffer = std::move(payload);
            continue;
        }
        if (opcode == kOpContinuation) {
            if (conn->fragmentOpcode == 0)
                return failConnection(conn, 1002, "continuation without a start");
            if (conn->fragmentBuffer.size() + payload.size() > kMaxMessageBytes)
                return failConnection(conn, 1009, "message too big");
            conn->fragmentBuffer += payload;
            if (fin) {
                std::string message = std::move(conn->fragmentBuffer);
                conn->fragmentBuffer.clear();
                conn->fragmentOpcode = 0;
                if (m_callbacks.textMessage)
                    m_callbacks.textMessage(conn->id, message);
            }
            continue;
        }
        return failConnection(conn, 1002, "unknown opcode");
    }
}

void WsServer::handleControlFrame(Conn *conn, std::uint8_t opcode, std::string_view payload)
{
    if (opcode == kOpPing) {
        sendFrame(conn, kOpPong, payload);
        return;
    }
    if (opcode == kOpPong)
        return;
    if (opcode == kOpClose) {
        if (!conn->sentClose) {
            conn->sentClose = true;
            sendFrame(conn, kOpClose, payload.substr(0, 2)); // echo the code
        }
        dropConn(conn);
    }
}

void WsServer::sendFrame(Conn *conn, std::uint8_t opcode, std::string_view payload)
{
    if (conn->closed)
        return;
    std::string frame;
    frame.reserve(payload.size() + 10);
    frame += static_cast<char>(0x80 | opcode);
    if (payload.size() < 126) {
        frame += static_cast<char>(payload.size());
    } else if (payload.size() <= 0xFFFF) {
        frame += static_cast<char>(126);
        frame += static_cast<char>((payload.size() >> 8) & 0xFF);
        frame += static_cast<char>(payload.size() & 0xFF);
    } else {
        frame += static_cast<char>(127);
        for (int i = 7; i >= 0; --i)
            frame += static_cast<char>((static_cast<std::uint64_t>(payload.size()) >> (8 * i)) & 0xFF);
    }
    frame += payload;
    conn->outBuffer += frame;
    flushConn(conn);
}

void WsServer::sendText(ConnId id, std::string_view text)
{
    auto it = m_conns.find(id);
    if (it == m_conns.end() || !it->second->open)
        return;
    sendFrame(it->second.get(), kOpText, text);
}

std::vector<WsServer::ConnId> WsServer::connectionIds() const
{
    std::vector<ConnId> ids;
    ids.reserve(m_conns.size());
    for (const auto &entry : m_conns) {
        if (!entry.second->closed)
            ids.push_back(entry.first);
    }
    return ids;
}

void WsServer::closeConnection(ConnId id, std::uint16_t code, std::string_view reason)
{
    auto it = m_conns.find(id);
    if (it == m_conns.end())
        return;
    Conn *conn = it->second.get();
    if (!conn->sentClose) {
        conn->sentClose = true;
        std::string payload;
        payload += static_cast<char>((code >> 8) & 0xFF);
        payload += static_cast<char>(code & 0xFF);
        payload += reason;
        sendFrame(conn, kOpClose, payload);
    }
    dropConn(conn);
}

void WsServer::failConnection(Conn *conn, std::uint16_t code, std::string_view reason)
{
    if (!conn->sentClose) {
        conn->sentClose = true;
        std::string payload;
        payload += static_cast<char>((code >> 8) & 0xFF);
        payload += static_cast<char>(code & 0xFF);
        payload += reason;
        sendFrame(conn, kOpClose, payload);
    }
    dropConn(conn);
}

void WsServer::flushConn(Conn *conn)
{
    while (!conn->outBuffer.empty()) {
        const ssize_t sent = ::write(conn->fd, conn->outBuffer.data(), conn->outBuffer.size());
        if (sent > 0) {
            conn->outBuffer.erase(0, static_cast<std::size_t>(sent));
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            conn->writeWatch.setEnabled(true);
            return;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        dropConn(conn);
        return;
    }
    conn->writeWatch.setEnabled(false);
}

void WsServer::onWritable(Conn *conn)
{
    if (conn->closed)
        return;
    flushConn(conn);
}

void WsServer::dropConn(Conn *conn)
{
    if (conn->closed)
        return;
    auto it = m_conns.find(conn->id);
    if (it == m_conns.end())
        return;
    std::shared_ptr<Conn> victim = std::move(it->second);
    m_conns.erase(it);
    victim->closed = true;

    const bool wasOpen = victim->open;
    const ConnId id = victim->id;

    // The watches die on the next loop turn: this may run inside one of their
    // own callbacks, and a watch must not be reset from inside itself.
    m_loop->post([victim]() {
        victim->readWatch.reset();
        victim->writeWatch.reset();
        ::close(victim->fd);
    });

    if (wasOpen && m_callbacks.disconnected)
        m_callbacks.disconnected(id);
}

} // namespace phicore::transport::ws
