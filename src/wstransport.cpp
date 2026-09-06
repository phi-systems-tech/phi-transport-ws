#include "wstransport.h"

#include <algorithm>
#include <cctype>
#include <chrono>

namespace phicore::transport::ws {

namespace {

// Envelope types and the topics a transport produces itself come from
// envelope.h - they are protocol surface, and two transports owning a copy
// each is how the wire drifts.
constexpr std::uint16_t kDefaultPort = 5040;

// How often idle sessions are looked at. The budget itself comes from core;
// this only decides how late the close may be, and a few seconds on a timeout
// counted in minutes is not worth a timer per connection.
constexpr int kIdleSweepIntervalMs = 5000;

std::int64_t wallClockMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string trimmedCopy(std::string text)
{
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.pop_back();
    std::size_t start = 0;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t'))
        ++start;
    return text.substr(start);
}

std::string lowered(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool equalsIgnoreCase(const std::string &a, const std::string &b)
{
    return lowered(a) == lowered(b);
}

} // namespace

WsTransport::~WsTransport()
{
    stop();
}

std::string WsTransport::pluginType() const
{
    return "ws";
}

std::string WsTransport::displayName() const
{
    return "WebSocket";
}

std::string WsTransport::description() const
{
    return "WebSocket transport plugin for phi-core APIs.";
}

bool WsTransport::start(std::string_view configJson, std::string *errorString)
{
    const auto fail = [&](std::string message) {
        if (errorString)
            *errorString = std::move(message);
        return false;
    };

    const Json config = Json::parse(configJson, nullptr, false);
    std::string configError;
    if (!isConfigValid(config, &configError))
        return fail(std::move(configError));

    if (m_running)
        stop();

    const std::string host = hostFromConfig(config);
    const std::uint16_t port = portFromConfig(config);

    WsServer::Callbacks callbacks;
    callbacks.acceptOrigin = [this](const std::string &origin) { return acceptOrigin(origin); };
    callbacks.connected = [this](ConnId id, const std::string &peer, std::uint16_t peerPort) {
        onConnected(id, peer, peerPort);
    };
    callbacks.disconnected = [this](ConnId id) { onDisconnected(id); };
    callbacks.textMessage = [this](ConnId id, std::string_view text) { onTextMessage(id, text); };

    std::string listenError;
    // UI clients request the protocol string "phi-core-ws.v1". Without an
    // agreed subprotocol, browser WebSocket clients reject the handshake.
    if (!m_server.listen(*runtimeLoop(), host, port, "phi-core-ws.v1",
                         std::move(callbacks), &listenError))
        return fail(std::move(listenError));

    m_allowedOrigins = allowedOriginsFromConfig(config);
    m_idleSweep = runtimeLoop()->timerEvery(std::chrono::milliseconds(kIdleSweepIntervalMs),
                                            [this]() { dropIdleSessions(); });
    m_running = true;
    writeLog(LogLevel::Info,
             makeCategory(LogCategory::Transport),
             "WS transport started on %1:%2",
             {Scalar{host}, Scalar{static_cast<std::int64_t>(port)}},
             "ws.start",
             jsonObject({{"host", jsonQuoted(host)},
                         {"port", std::to_string(port)}}));
    return true;
}

void WsTransport::stop()
{
    if (!m_running && !m_server.isListening())
        return;

    m_idleSweep.reset();
    m_disconnectAll.reset();
    m_server.close();
    m_sessions.clear();
    m_pendingCommands.clear();
    m_running = false;
}

void WsTransport::onCoreAsyncResult(CmdId cmdId, std::string_view payloadJson)
{
    auto it = m_pendingCommands.find(cmdId);
    if (it == m_pendingCommands.end())
        return;

    const PendingCommand pending = it->second;
    m_pendingCommands.erase(it);
    sendCmdResponse(pending.conn, pending.cid, pending.cmdTopic, payloadJson);
}

void WsTransport::onCoreEvent(std::string_view topic, std::string_view payloadJson)
{
    if (topic.empty())
        return;
    ++m_eventsSinceLast;
    if (topic == std::string_view("event.channel.stateChanged"))
        ++m_channelEventsSinceLast;
    const std::int64_t now = wallClockMs();
    if (m_lastStatsLogMs <= 0 || (now - m_lastStatsLogMs) >= 5000) {
        writeLog(LogLevel::Debug,
                 makeCategory(LogCategory::Transport),
                 "WS broadcast stats: clients=%1 events=%2 channelEvents=%3",
                 {Scalar{static_cast<std::int64_t>(m_server.connectionCount())},
                  Scalar{static_cast<std::int64_t>(m_eventsSinceLast)},
                  Scalar{static_cast<std::int64_t>(m_channelEventsSinceLast)}},
                 "ws.broadcastStats",
                 jsonObject({{"clients", std::to_string(m_server.connectionCount())},
                             {"events", std::to_string(m_eventsSinceLast)},
                             {"channelEvents", std::to_string(m_channelEventsSinceLast)}}));
        m_eventsSinceLast = 0;
        m_channelEventsSinceLast = 0;
        m_lastStatsLogMs = now;
    }
    broadcastEvent(topic, payloadJson);
}

bool WsTransport::acceptOrigin(const std::string &originRaw)
{
    const std::string origin = trimmedCopy(originRaw);
    if (origin.empty()) {
        // No Origin header: not a browser. Command-line clients and services
        // are unaffected by this check.
        return true;
    }
    if (isLoopbackOrigin(origin))
        return true;
    for (const std::string &allowed : m_allowedOrigins) {
        if (equalsIgnoreCase(allowed, origin))
            return true;
    }
    writeLog(LogLevel::Warn,
             makeCategory(LogCategory::Security, true),
             "Refused a WebSocket handshake from origin %1; list it under 'allowedOrigins' in the transport config if it is yours",
             {Scalar{origin}},
             "ws.originRefused",
             jsonObject({{"origin", jsonQuoted(origin)}}));
    return false;
}

void WsTransport::onConnected(ConnId id, const std::string &peerAddress, std::uint16_t peerPort)
{
    writeLog(LogLevel::Info,
             makeCategory(LogCategory::Transport),
             "WS client connected: %1:%2 total=%3",
             {Scalar{peerAddress},
              Scalar{static_cast<std::int64_t>(peerPort)},
              Scalar{static_cast<std::int64_t>(m_server.connectionCount())}},
             "ws.clientConnected",
             jsonObject({{"peerAddress", jsonQuoted(peerAddress)},
                         {"peerPort", std::to_string(peerPort)},
                         {"clientCount", std::to_string(m_server.connectionCount())}}));
}

void WsTransport::onDisconnected(ConnId id)
{
    m_sessions.erase(id);
    for (auto it = m_pendingCommands.begin(); it != m_pendingCommands.end();) {
        if (it->second.conn == id)
            it = m_pendingCommands.erase(it);
        else
            ++it;
    }
    writeLog(LogLevel::Info,
             makeCategory(LogCategory::Transport),
             "WS client disconnected: total=%1",
             {Scalar{static_cast<std::int64_t>(m_server.connectionCount())}},
             "ws.clientDisconnected",
             jsonObject({{"clientCount", std::to_string(m_server.connectionCount())}}));
}

void WsTransport::onTextMessage(ConnId id, std::string_view message)
{
    const Json doc = Json::parse(message, nullptr, false);
    if (!doc.is_object()) {
        sendProtocolError(id, std::nullopt, kErrorCodeInvalidJson, kMessageInvalidJson);
        return;
    }

    const std::string type = doc.value("type", std::string());
    const std::string topic = trimmedCopy(doc.value("topic", std::string()));
    const Json payload = doc.contains("payload") && doc["payload"].is_object()
        ? doc["payload"] : Json::object();

    const std::optional<CmdId> cid = readCid(doc.contains("cid") ? doc["cid"] : Json());
    if (!cid.has_value()) {
        sendProtocolError(id, std::nullopt, kErrorCodeMissingCid, kMessageMissingCid);
        return;
    }

    if (type != kEnvelopeTypeCmd) {
        sendProtocolError(id, cid, kErrorCodeInvalidType, kMessageInvalidType);
        return;
    }

    if (topic.empty()) {
        sendProtocolError(id, cid, kErrorCodeMissingTopic, kMessageMissingTopic);
        return;
    }

    // A connection that has not authenticated gets the handshake and the login,
    // and nothing else. Core would refuse the rest anyway, but a socket that
    // answers to anyone should not be able to make it do the refusing (F-42).
    const std::string requestClientId = payload.value("clientId", std::string());
    if (m_sessions.find(id) == m_sessions.end() && !isPreAuthTopic(topic)) {
        sendProtocolError(id, cid, "unauthenticated",
                          "Authenticate with sync.auth.begin.set and sync.auth.login.set"
                          " before sending this topic.");
        return;
    }
    // What counts as activity is what core counts: a call it authorizes, which
    // is where it touches the session. The pre-auth topics are not that - a
    // heartbeat says the socket is open, not that anyone is still using it, and
    // letting it extend the session would make the timeout decorative.
    if (!isPreAuthTopic(topic)) {
        if (auto session = m_sessions.find(id); session != m_sessions.end())
            session->second.lastActivityMs = wallClockMs();
    }

    // Only used to remember a session the client already held when it said hello.
    const std::string requestAuthToken = trimmedCopy(payload.value("authToken", std::string()));

    // The API takes the payload as text; this transport parsed the frame to read the
    // envelope, so the sub-object is serialized once here. That extra step is the
    // cost side of the text boundary, and it sits on the command path rather than on
    // the event path.
    handleCommand(id, *cid, topic, requestClientId, requestAuthToken, payload.dump());
}

bool WsTransport::isConfigValid(const Json &config, std::string *errorString)
{
    const int port = config.is_object()
        ? config.value("port", static_cast<int>(kDefaultPort))
        : static_cast<int>(kDefaultPort);
    if (port < 1 || port > 65535) {
        if (errorString)
            *errorString = "Invalid 'port' value; expected 1..65535.";
        return false;
    }

    if (hostFromConfig(config).empty()) {
        if (errorString)
            *errorString = "Invalid 'host' value.";
        return false;
    }

    return true;
}

std::optional<CmdId> WsTransport::readCid(const Json &value)
{
    if (value.is_number())
        return cidFromNumber(value.get<double>());
    if (value.is_string())
        return cidFromString(value.get<std::string>());
    return std::nullopt;
}

std::vector<std::string> WsTransport::allowedOriginsFromConfig(const Json &config)
{
    std::vector<std::string> origins;
    if (!config.is_object())
        return origins;
    const auto it = config.find("allowedOrigins");
    if (it == config.end() || !it->is_array())
        return origins;
    for (const Json &entry : *it) {
        if (!entry.is_string())
            continue;
        std::string origin = trimmedCopy(entry.get<std::string>());
        if (!origin.empty())
            origins.push_back(std::move(origin));
    }
    return origins;
}

bool WsTransport::isLoopbackOrigin(const std::string &origin)
{
    // A UI served from the same machine keeps working out of the box, whichever
    // port a dev server or the packaged UI happens to use. Anything else has to
    // be named. That is the line between "the operator's own page" and
    // "whatever site the browser happens to have open".
    const std::string low = lowered(origin);
    std::string rest;
    if (low.rfind("http://", 0) == 0)
        rest = low.substr(7);
    else if (low.rfind("https://", 0) == 0)
        rest = low.substr(8);
    else
        return false;

    // The host part: up to the port or the path, brackets stripped for IPv6.
    std::string hostPart = rest.substr(0, rest.find('/'));
    if (!hostPart.empty() && hostPart.front() == '[') {
        const std::size_t closing = hostPart.find(']');
        if (closing == std::string::npos)
            return false;
        hostPart = hostPart.substr(1, closing - 1);
    } else {
        hostPart = hostPart.substr(0, hostPart.find(':'));
    }

    if (hostPart == "localhost" || hostPart == "::1")
        return true;
    // 127.0.0.0/8 - the whole block is loopback.
    return hostPart.rfind("127.", 0) == 0;
}

bool WsTransport::isPreAuthTopic(std::string_view topic)
{
    // The handshake, the way in, and the way out. Core owns the authoritative
    // table and refuses anything else anyway; this list exists so an
    // unauthenticated flood never reaches it in the first place.
    return topic == "sync.hello.get"
        || topic == "sync.ping.get"
        || topic.rfind("sync.auth.", 0) == 0;
}

void WsTransport::trackAuthOutcome(ConnId id,
                                   const std::string &topic,
                                   const std::string &requestClientId,
                                   const std::string &requestAuthToken,
                                   std::string_view responsePayloadJson)
{
    if (topic == "sync.auth.logout.set") {
        m_sessions.erase(id);
        return;
    }

    const bool isLogin = topic == "sync.auth.login.set"
        || topic == "sync.auth.bootstrap.set"
        || topic == "sync.hello.get";
    if (!isLogin)
        return;

    // The only place this transport looks inside a payload: the session core
    // just issued is what it has to remember, and it is in the answer.
    const Json response = Json::parse(responsePayloadJson, nullptr, false);
    if (!response.is_object())
        return;

    // How long this session may sit idle is core's decision, and it states it in
    // the same answer that hands out the token (F-42). 0 or absent means core
    // does not expire sessions, so neither does this transport.
    const std::int64_t idleBudgetMs =
        static_cast<std::int64_t>(response.value("sessionIdleSec", 0.0)) * 1000;
    const std::int64_t now = wallClockMs();

    const std::string token = trimmedCopy(response.value("token", std::string()));
    if (!token.empty()) {
        ClientSession session;
        session.token = token;
        session.clientId = requestClientId;
        session.idleBudgetMs = idleBudgetMs;
        session.lastActivityMs = now;
        m_sessions[id] = std::move(session);
        return;
    }

    // hello with an authToken core accepted: the client already had a session.
    if (topic == "sync.hello.get" && response.value("authAccepted", false)) {
        if (requestAuthToken.empty())
            return;
        ClientSession session;
        session.token = requestAuthToken;
        session.clientId = requestClientId;
        session.idleBudgetMs = idleBudgetMs;
        session.lastActivityMs = now;
        m_sessions[id] = std::move(session);
    }
}

std::string WsTransport::hostFromConfig(const Json &config)
{
    if (config.is_object()) {
        const std::string host = trimmedCopy(config.value("host", std::string()));
        if (!host.empty())
            return host;
    }
    return "127.0.0.1";
}

std::uint16_t WsTransport::portFromConfig(const Json &config)
{
    const int port = config.is_object()
        ? config.value("port", static_cast<int>(kDefaultPort))
        : static_cast<int>(kDefaultPort);
    if (port < 1 || port > 65535)
        return kDefaultPort;
    return static_cast<std::uint16_t>(port);
}

namespace {

// The two actions, by id. Strings the UI shows are English here and
// translated there, like every adapter's.
constexpr std::string_view kActionSessions = "sessions";
constexpr std::string_view kActionDisconnectAll = "disconnectAll";

} // namespace

JsonText WsTransport::describeManagement() const
{
    const std::size_t clients = m_server.connectionCount();
    const std::size_t sessions = m_sessions.size();
    std::string summary;
    if (!m_running) {
        summary = "Not listening";
    } else {
        summary = std::to_string(clients) + (clients == 1 ? " client" : " clients") + ", "
            + std::to_string(sessions) + (sessions == 1 ? " session" : " sessions");
    }
    const JsonText sessionsAction = makeActionDescriptor(
        kActionSessions, "Show sessions", "Who is logged in over this transport, and for how long they have been idle.");
    const JsonText disconnectAction = makeActionDescriptor(
        kActionDisconnectAll, "Disconnect all clients",
        "Closes every WebSocket connection, this one included. Clients reconnect and log in again.",
        "\"danger\":true,\"confirm\":" + jsonObject({{"title", jsonQuoted("Disconnect every client?")},
                                                       {"okText", jsonQuoted("Disconnect")}}));
    return makeManagementDescription(summary, {sessionsAction, disconnectAction});
}

bool WsTransport::invokeAction(CmdId cmdId, std::string_view actionId, std::string_view paramsJson)
{
    (void)paramsJson;
    if (actionId == kActionSessions) {
        if (m_sessions.empty()) {
            completeAction(cmdId, makeActionResultText("No session is logged in."));
            return true;
        }
        const std::int64_t now = wallClockMs();
        std::string text;
        for (const auto &entry : m_sessions) {
            if (!text.empty())
                text += '\n';
            const std::int64_t idleSec = entry.second.lastActivityMs > 0 ? (now - entry.second.lastActivityMs) / 1000 : 0;
            text += entry.second.clientId.empty() ? std::string("(no client id)") : entry.second.clientId;
            text += ": idle " + std::to_string(idleSec) + " s";
        }
        completeAction(cmdId, makeActionResultText(text));
        return true;
    }
    if (actionId == kActionDisconnectAll) {
        const std::vector<ConnId> ids = m_server.connectionIds();
        // The answer goes out before the door shuts, or the asker never sees
        // it: the connection it asked on is among the ones being closed.
        completeAction(cmdId,
                       makeActionResultText(std::to_string(ids.size())
                                                + (ids.size() == 1 ? " client disconnected." : " clients disconnected."),
                                            true));
        m_disconnectAll = runtimeLoop()->timerAfter(std::chrono::milliseconds(250), [this, ids]() {
            for (const ConnId id : ids)
                m_server.closeConnection(id, 1001, "Disconnected by an administrator");
        });
        return true;
    }
    return false;
}

void WsTransport::dropIdleSessions()
{
    const std::int64_t now = wallClockMs();
    std::vector<std::pair<ConnId, ClientSession>> expired;
    for (const auto &entry : m_sessions) {
        if (entry.second.idleBudgetMs <= 0)
            continue;
        if (now - entry.second.lastActivityMs > entry.second.idleBudgetMs)
            expired.emplace_back(entry.first, entry.second);
    }

    for (const auto &[id, session] : expired) {
        m_sessions.erase(id);
        const std::int64_t idleSec = (now - session.lastActivityMs) / 1000;
        writeLog(LogLevel::Info,
                 makeCategory(LogCategory::Security),
                 "Closing an idle connection after %1 s without a call (client '%2')",
                 {Scalar{idleSec}, Scalar{session.clientId}},
                 "ws.idleTimeout",
                 jsonObject({{"idleSec", std::to_string(idleSec)},
                             {"clientId", jsonQuoted(session.clientId)}}));
        // Core drops the token on the same clock; this closes the pipe that
        // would otherwise keep pushing events at a session nobody is watching.
        m_server.closeConnection(id, 1000, "Session idle timeout");
    }
}

void WsTransport::send(ConnId id,
                       std::string_view type,
                       std::string_view topic,
                       std::optional<CmdId> cid,
                       std::string_view payloadJson)
{
    // The envelope shape comes from the shared header; the payload is spliced as
    // text, so an event that core serialized once travels straight to the wire.
    const JsonText out = makeEnvelope(type, topic, cid, payloadJson);
    m_server.sendText(id, out);
}

void WsTransport::sendProtocolError(ConnId id,
                                    std::optional<CmdId> cid,
                                    std::string_view code,
                                    std::string_view message)
{
    send(id, kEnvelopeTypeError, kTopicProtocolError, cid, makeProtocolErrorPayload(code, message));
}

void WsTransport::sendCmdResponse(ConnId id,
                                  CmdId cid,
                                  const std::string &cmdTopic,
                                  std::string_view payloadJson)
{
    // The only outbound path that parses: it adds `error: null` *if absent*, and
    // deciding that from raw text would be a substring guess. Command responses are
    // user-driven, so one parse here is the cheap side of the trade.
    Json out = Json::parse(payloadJson, nullptr, false);
    if (!out.is_object())
        out = Json::object();
    out["cmd"] = cmdTopic;
    if (!out.contains("error"))
        out["error"] = nullptr;
    const std::string bytes = out.dump();
    send(id, kEnvelopeTypeResponse, kTopicCmdResponse, cid, bytes);
}

void WsTransport::broadcastEvent(std::string_view topic, std::string_view payloadJson)
{
    // No cid on events; otherwise the same envelope as everything else.
    //
    // Events carry live state - channel values, adapter status - so they go only
    // to connections that logged in. Otherwise anything that can open a
    // connection would read the house without ever authenticating, which is the
    // same leak the command gate closes (F-42). The envelope is built once for
    // the whole fan-out.
    if (m_sessions.empty())
        return;
    const JsonText out = makeEnvelope(kEnvelopeTypeEvent, topic, std::nullopt, payloadJson);
    std::vector<ConnId> ids;
    ids.reserve(m_sessions.size());
    for (const auto &entry : m_sessions) {
        if (!entry.second.token.empty())
            ids.push_back(entry.first);
    }
    for (ConnId id : ids)
        m_server.sendText(id, out);
}

void WsTransport::handleCommand(ConnId id,
                                CmdId cid,
                                const std::string &topic,
                                const std::string &requestClientId,
                                const std::string &requestAuthToken,
                                std::string_view payloadJson)
{
    // Routing is the protocol's decision, made once in TransportPluginBase. What
    // is left here is what only this transport knows: which client asked, and how
    // to frame the answer.
    //
    // The identity comes from the connection, not from the frame: a client cannot
    // hand itself a session by putting a token in a payload (F-42, F-60).
    CallerIdentity caller;
    if (const auto sessionIt = m_sessions.find(id); sessionIt != m_sessions.end()
        && !sessionIt->second.token.empty()) {
        caller.kind = CallerIdentity::Kind::Session;
        caller.sessionToken = sessionIt->second.token;
        caller.clientId = sessionIt->second.clientId;
    }
    const CommandOutcome outcome = dispatchCommand(topic, payloadJson, caller);

    // A login, a bootstrap or a hello that core accepted establishes the session
    // this connection speaks with from now on.
    trackAuthOutcome(id, topic, requestClientId, requestAuthToken, outcome.payloadJson);

    if (outcome.cmdId > 0) {
        // Core took the command and answers later; the client waits under that id
        // until onCoreAsyncResult arrives.
        PendingCommand pending;
        pending.conn = id;
        pending.cid = cid;
        pending.cmdTopic = topic;
        m_pendingCommands.emplace(outcome.cmdId, pending);
    }

    const auto [type, envelopeTopic] = envelopeFor(outcome.kind);
    send(id, type, envelopeTopic, cid, outcome.payloadJson);
}

} // namespace phicore::transport::ws

PHI_TRANSPORT_PLUGIN(phicore::transport::ws::WsTransport)
