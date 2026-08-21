#include "wstransport.h"

#include <QDateTime>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketCorsAuthenticator>
#include <QWebSocketProtocol>
#include <QWebSocketServer>

namespace phicore::transport::ws {

namespace {

// Envelope types and the topics a transport produces itself now come from
// envelope.h - they are protocol surface, and two transports owning a copy each
// is how the wire drifts.
constexpr quint16 kDefaultPort = 5040;

// How often idle sessions are looked at. The budget itself comes from core; this
// only decides how late the close may be, and a few seconds on a timeout counted
// in minutes is not worth a timer per connection.
constexpr int kIdleSweepIntervalMs = 5000;

} // namespace

WsTransport::WsTransport(QObject *parent)
    : QObject(parent)
{
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
    // The private helpers below stay in QString; only the contract is Qt-free,
    // and converting once here beats threading std::string through them.
    QString localError;
    const auto reportError = [&]() {
        if (errorString)
            *errorString = localError.toStdString();
        return false;
    };
    // Config arrives as JSON text; parsed once here, then used as an object as
    // before.
    const QJsonObject config =
        QJsonDocument::fromJson(QByteArray::fromRawData(configJson.data(),
                                                       static_cast<qsizetype>(configJson.size())))
            .object();
    if (!isConfigValid(config, &localError))
        return reportError();

    if (m_running)
        stop();

    const QString host = hostFromConfig(config);
    const quint16 port = portFromConfig(config);
    if (!startServer(host, port, &localError))
        return reportError();

    m_config = config;
    m_allowedOrigins = allowedOriginsFromConfig(config);
    if (!m_idleSweep) {
        m_idleSweep = new QTimer(this);
        m_idleSweep->setInterval(kIdleSweepIntervalMs);
        connect(m_idleSweep, &QTimer::timeout, this, &WsTransport::dropIdleSessions);
    }
    m_idleSweep->start();
    m_running = true;
    const std::string hostText = host.toStdString();
    writeLog(LogLevel::Info,
             makeCategory(LogCategory::Transport),
             "WS transport started on %1:%2",
             {Scalar{hostText}, Scalar{static_cast<std::int64_t>(port)}},
             "ws.start",
             jsonObject({{"host", jsonQuoted(hostText)},
                         {"port", std::to_string(port)}}));
    return true;
}

void WsTransport::stop()
{
    if (!m_running && !m_server)
        return;

    if (m_idleSweep)
        m_idleSweep->stop();
    closeAllClients();
    m_clients.clear();
    m_sessions.clear();
    m_pendingCommands.clear();

    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }

    m_running = false;
}

void WsTransport::onCoreAsyncResult(CmdId cmdId, std::string_view payloadJson)
{
    auto it = m_pendingCommands.find(cmdId);
    if (it == m_pendingCommands.end())
        return;

    const PendingCommand pending = it.value();
    m_pendingCommands.erase(it);

    QWebSocket *socket = pending.socket.data();
    if (!socket || socket->state() != QAbstractSocket::ConnectedState)
        return;

    sendCmdResponse(socket, pending.cid, pending.cmdTopic, payloadJson);
}

void WsTransport::onCoreEvent(std::string_view topic, std::string_view payloadJson)
{
    const QString topicText = QString::fromUtf8(topic.data(), static_cast<qsizetype>(topic.size()));
    if (topicText.trimmed().isEmpty())
        return;
    static qint64 s_lastStatsLogMs = 0;
    static quint64 s_eventsSinceLast = 0;
    static quint64 s_channelEventsSinceLast = 0;
    ++s_eventsSinceLast;
    if (topic == std::string_view("event.channel.stateChanged"))
        ++s_channelEventsSinceLast;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (s_lastStatsLogMs <= 0 || (nowMs - s_lastStatsLogMs) >= 5000) {
        QJsonObject fields;
        const std::string clients = std::to_string(m_clients.size());
        const std::string events = std::to_string(s_eventsSinceLast);
        const std::string channelEvents = std::to_string(s_channelEventsSinceLast);
        writeLog(LogLevel::Debug,
                 makeCategory(LogCategory::Transport),
                 "WS broadcast stats: clients=%1 events=%2 channelEvents=%3",
                 {Scalar{static_cast<std::int64_t>(m_clients.size())},
                  Scalar{static_cast<std::int64_t>(s_eventsSinceLast)},
                  Scalar{static_cast<std::int64_t>(s_channelEventsSinceLast)}},
                 "ws.broadcastStats",
                 jsonObject({{"clients", clients},
                             {"events", events},
                             {"channelEvents", channelEvents}}));
        s_eventsSinceLast = 0;
        s_channelEventsSinceLast = 0;
        s_lastStatsLogMs = nowMs;
    }
    broadcastEvent(topic, payloadJson);
}

void WsTransport::onNewConnection()
{
    if (!m_server)
        return;

    while (m_server->hasPendingConnections()) {
        QWebSocket *socket = m_server->nextPendingConnection();
        if (!socket)
            continue;
        m_clients.insert(socket);
        const QString peerAddress = socket->peerAddress().toString();
        const int peerPort = socket->peerPort();
        const std::string peerText = peerAddress.toStdString();
        const std::string portText = std::to_string(peerPort);
        const std::string countText = std::to_string(m_clients.size());
        writeLog(LogLevel::Info,
                 makeCategory(LogCategory::Transport),
                 "WS client connected: %1:%2 total=%3",
                 {Scalar{peerText},
                  Scalar{static_cast<std::int64_t>(peerPort)},
                  Scalar{static_cast<std::int64_t>(m_clients.size())}},
                 "ws.clientConnected",
                 jsonObject({{"peerAddress", jsonQuoted(peerText)},
                             {"peerPort", portText},
                             {"clientCount", countText}}));
        connect(socket, &QWebSocket::textMessageReceived,
                this, &WsTransport::onTextMessageReceived);
        connect(socket, &QWebSocket::disconnected,
                this, &WsTransport::onSocketDisconnected);
    }
}

void WsTransport::onSocketDisconnected()
{
    auto *socket = qobject_cast<QWebSocket *>(sender());
    if (!socket)
        return;

    m_clients.remove(socket);
    m_sessions.remove(socket);
    const QString peerAddress = socket->peerAddress().toString();
    const int peerPort = socket->peerPort();
    QJsonObject fields;
    const std::string peerText = peerAddress.toStdString();
    const std::string portText = std::to_string(peerPort);
    const std::string countText = std::to_string(m_clients.size());
    writeLog(LogLevel::Info,
             makeCategory(LogCategory::Transport),
             "WS client disconnected: %1:%2 total=%3",
             {Scalar{peerText},
              Scalar{static_cast<std::int64_t>(peerPort)},
              Scalar{static_cast<std::int64_t>(m_clients.size())}},
             "ws.clientDisconnected",
             jsonObject({{"peerAddress", jsonQuoted(peerText)},
                         {"peerPort", portText},
                         {"clientCount", countText}}));

    for (auto it = m_pendingCommands.begin(); it != m_pendingCommands.end();) {
        if (it.value().socket == socket)
            it = m_pendingCommands.erase(it);
        else
            ++it;
    }

    socket->deleteLater();
}

void WsTransport::onTextMessageReceived(const QString &message)
{
    auto *socket = qobject_cast<QWebSocket *>(sender());
    if (!socket)
        return;


    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        sendProtocolError(socket, std::nullopt, kErrorCodeInvalidJson, kMessageInvalidJson);
        return;
    }

    const QJsonObject obj = doc.object();
    const QString type = obj.value(QStringLiteral("type")).toString();
    const QString topic = obj.value(QStringLiteral("topic")).toString();
    const QJsonObject payload = obj.value(QStringLiteral("payload")).toObject();

    const std::optional<CmdId> cid = readCid(obj.value(QStringLiteral("cid")));
    if (!cid.has_value()) {
        sendProtocolError(socket, std::nullopt, kErrorCodeMissingCid, kMessageMissingCid);
        return;
    }

    if (type.toStdString() != kEnvelopeTypeCmd) {
        sendProtocolError(socket, cid, kErrorCodeInvalidType, kMessageInvalidType);
        return;
    }

    if (topic.trimmed().isEmpty()) {
        sendProtocolError(socket, cid, kErrorCodeMissingTopic, kMessageMissingTopic);
        return;
    }

    // A connection that has not authenticated gets the handshake and the login,
    // and nothing else. Core would refuse the rest anyway, but a socket that
    // answers to anyone should not be able to make it do the refusing (F-42).
    const QString requestClientId = payload.value(QStringLiteral("clientId")).toString();
    if (!m_sessions.contains(socket) && !isPreAuthTopic(topic)) {
        sendProtocolError(socket, cid, "unauthenticated",
                          "Authenticate with sync.auth.login.set before sending this topic.");
        return;
    }
    // What counts as activity is what core counts: a call it authorizes, which
    // is where it touches the session. The pre-auth topics are not that - a
    // heartbeat says the socket is open, not that anyone is still using it, and
    // letting it extend the session would make the timeout decorative.
    if (!isPreAuthTopic(topic)) {
        if (auto session = m_sessions.find(socket); session != m_sessions.end())
            session->lastActivityMs = QDateTime::currentMSecsSinceEpoch();
    }

    // Only used to remember a session the client already held when it said hello.
    const QString requestAuthToken = payload.value(QStringLiteral("authToken")).toString().trimmed();

    // The API takes the payload as text; this transport parsed the frame to read the
    // envelope, so the sub-object is serialized once here. That extra step is the
    // cost side of the text boundary, and it sits on the command path rather than on
    // the event path.
    const QByteArray payloadBytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    handleCommand(socket,
                  *cid,
                  topic,
                  requestClientId,
                  requestAuthToken,
                  std::string_view(payloadBytes.constData(), static_cast<std::size_t>(payloadBytes.size())));
}

bool WsTransport::isConfigValid(const QJsonObject &config, QString *errorString)
{
    const int port = static_cast<int>(portFromConfig(config));
    if (port < 1 || port > 65535) {
        if (errorString)
            *errorString = QStringLiteral("Invalid 'port' value; expected 1..65535.");
        return false;
    }

    const QString host = hostFromConfig(config).trimmed();
    if (host.isEmpty()) {
        if (errorString)
            *errorString = QStringLiteral("Invalid 'host' value.");
        return false;
    }

    return true;
}

std::optional<CmdId> WsTransport::readCid(const QJsonValue &value)
{
    if (value.isDouble())
        return cidFromNumber(value.toDouble(-1.0));
    if (value.isString())
        return cidFromString(value.toString().toStdString());
    return std::nullopt;
}

QStringList WsTransport::allowedOriginsFromConfig(const QJsonObject &config)
{
    QStringList origins;
    const QJsonValue configured = config.value(QStringLiteral("allowedOrigins"));
    if (configured.isArray()) {
        const QJsonArray entries = configured.toArray();
        for (const QJsonValue &entry : entries) {
            const QString origin = entry.toString().trimmed();
            if (!origin.isEmpty())
                origins.append(origin);
        }
    }
    return origins;
}

bool WsTransport::isLoopbackOrigin(const QString &origin)
{
    // A UI served from the same machine keeps working out of the box, whichever
    // port a dev server or the packaged UI happens to use. Anything else has to
    // be named. That is the line between "the operator's own page" and
    // "whatever site the browser happens to have open".
    const QUrl url(origin);
    if (!url.isValid())
        return false;
    const QString scheme = url.scheme().toLower();
    if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https"))
        return false;

    const QString host = url.host().toLower();
    if (host == QStringLiteral("localhost") || host == QStringLiteral("::1"))
        return true;
    const QHostAddress address(host);
    return !address.isNull() && address.isLoopback();
}

bool WsTransport::isPreAuthTopic(const QString &topic)
{
    // The handshake, the way in, and the way out. Core owns the authoritative
    // table and refuses anything else anyway; this list exists so an
    // unauthenticated flood never reaches it in the first place.
    return topic == QLatin1String("sync.hello.get")
        || topic == QLatin1String("sync.ping.get")
        || topic.startsWith(QLatin1String("sync.auth."));
}

void WsTransport::trackAuthOutcome(QWebSocket *socket,
                                   const QString &topic,
                                   const QString &requestClientId,
                                   const QString &requestAuthToken,
                                   std::string_view responsePayloadJson)
{
    if (!socket)
        return;

    if (topic == QLatin1String("sync.auth.logout.set")) {
        m_sessions.remove(socket);
        return;
    }

    const bool isLogin = topic == QLatin1String("sync.auth.login.set")
        || topic == QLatin1String("sync.auth.bootstrap.set")
        || topic == QLatin1String("sync.hello.get");
    if (!isLogin)
        return;

    // The only place this transport looks inside a payload: the session core
    // just issued is what it has to remember, and it is in the answer.
    const QJsonObject response =
        QJsonDocument::fromJson(QByteArray::fromRawData(responsePayloadJson.data(),
                                                       static_cast<qsizetype>(responsePayloadJson.size())))
            .object();

    // How long this session may sit idle is core's decision, and it states it in
    // the same answer that hands out the token (F-42). 0 or absent means core
    // does not expire sessions, so neither does this transport.
    const qint64 idleBudgetMs =
        static_cast<qint64>(response.value(QStringLiteral("sessionIdleSec")).toDouble(0.0)) * 1000;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    const QString token = response.value(QStringLiteral("token")).toString().trimmed();
    if (!token.isEmpty()) {
        ClientSession session;
        session.token = token;
        session.clientId = requestClientId;
        session.idleBudgetMs = idleBudgetMs;
        session.lastActivityMs = nowMs;
        m_sessions.insert(socket, session);
        return;
    }

    // hello with an authToken core accepted: the client already had a session.
    if (topic == QLatin1String("sync.hello.get")
        && response.value(QStringLiteral("authAccepted")).toBool(false)) {
        ClientSession session;
        session.token = requestAuthToken;
        session.clientId = requestClientId;
        session.idleBudgetMs = idleBudgetMs;
        session.lastActivityMs = nowMs;
        if (!session.token.isEmpty())
            m_sessions.insert(socket, session);
    }
}

QString WsTransport::hostFromConfig(const QJsonObject &config)
{
    const QString host = config.value(QStringLiteral("host")).toString().trimmed();
    if (host.isEmpty())
        return QStringLiteral("127.0.0.1");
    return host;
}

quint16 WsTransport::portFromConfig(const QJsonObject &config)
{
    const int port = config.value(QStringLiteral("port")).toInt(static_cast<int>(kDefaultPort));
    if (port < 1 || port > 65535)
        return kDefaultPort;
    return static_cast<quint16>(port);
}

bool WsTransport::startServer(const QString &host, quint16 port, QString *errorString)
{
    auto *server = new QWebSocketServer(QStringLiteral("phi-transport-ws"),
                                        QWebSocketServer::NonSecureMode,
                                        this);
    // UI clients request the protocol string "phi-core-ws.v1". Without an
    // agreed subprotocol, browser WebSocket clients reject the handshake.
    server->setSupportedSubprotocols({ QStringLiteral("phi-core-ws.v1") });

    QHostAddress address;
    const QString normalizedHost = host.trimmed().toLower();
    if (normalizedHost == QStringLiteral("*")
        || normalizedHost == QStringLiteral("any")
        || normalizedHost == QStringLiteral("0.0.0.0")) {
        address = QHostAddress::AnyIPv4;
    } else if (normalizedHost == QStringLiteral("::")
               || normalizedHost == QStringLiteral("anyipv6")) {
        address = QHostAddress::AnyIPv6;
    } else if (normalizedHost == QStringLiteral("localhost")) {
        address = QHostAddress::LocalHost;
    } else if (!address.setAddress(host)) {
        delete server;
        if (errorString)
            *errorString = QStringLiteral("Invalid host address: %1").arg(host);
        return false;
    }

    if (!server->listen(address, port)) {
        const QString err = server->errorString();
        delete server;
        if (errorString)
            *errorString = err.isEmpty() ? QStringLiteral("Failed to listen on requested host/port") : err;
        return false;
    }

    // A WebSocket handshake is not subject to the same-origin policy, so any page
    // a browser has open can connect to this port unless the server checks the
    // Origin itself. Without this, "bound to loopback" protected nothing against
    // a website the user happened to visit (F-42).
    connect(server, &QWebSocketServer::originAuthenticationRequired,
            this, [this](QWebSocketCorsAuthenticator *authenticator) {
        if (!authenticator)
            return;
        const QString origin = authenticator->origin().trimmed();
        if (origin.isEmpty()) {
            // No Origin header: not a browser. Command-line clients and services
            // are unaffected by this check.
            authenticator->setAllowed(true);
            return;
        }
        if (isLoopbackOrigin(origin) || m_allowedOrigins.contains(origin, Qt::CaseInsensitive)) {
            authenticator->setAllowed(true);
            return;
        }
        authenticator->setAllowed(false);
        const std::string originText = origin.toStdString();
        writeLog(LogLevel::Warn,
                 makeCategory(LogCategory::Security, true),
                 "Refused a WebSocket handshake from origin %1; list it under 'allowedOrigins' in the transport config if it is yours",
                 {Scalar{originText}},
                 "ws.originRefused",
                 jsonObject({{"origin", jsonQuoted(originText)}}));
    });

    connect(server, &QWebSocketServer::newConnection,
            this, &WsTransport::onNewConnection);

    m_server = server;
    return true;
}

void WsTransport::dropIdleSessions()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QList<QWebSocket *> expired;
    for (auto it = m_sessions.constBegin(); it != m_sessions.constEnd(); ++it) {
        if (it->idleBudgetMs <= 0)
            continue;
        if (nowMs - it->lastActivityMs > it->idleBudgetMs)
            expired.append(it.key());
    }

    for (QWebSocket *socket : expired) {
        const ClientSession session = m_sessions.take(socket);
        if (!socket)
            continue;
        const std::string clientIdText = session.clientId.toStdString();
        const std::int64_t idleSec = (nowMs - session.lastActivityMs) / 1000;
        writeLog(LogLevel::Info,
                 makeCategory(LogCategory::Security),
                 "Closing an idle connection after %1 s without a call (client '%2')",
                 {Scalar{idleSec}, Scalar{clientIdText}},
                 "ws.idleTimeout",
                 jsonObject({{"idleSec", std::to_string(idleSec)},
                             {"clientId", jsonQuoted(clientIdText)}}));
        // Core drops the token on the same clock; this closes the pipe that
        // would otherwise keep pushing events at a session nobody is watching.
        socket->close(QWebSocketProtocol::CloseCodeNormal,
                      QStringLiteral("Session idle timeout"));
    }
}

void WsTransport::closeAllClients()
{
    const QList<QWebSocket *> clients = m_clients.values();
    for (QWebSocket *client : clients) {
        if (!client)
            continue;
        client->close();
        client->deleteLater();
    }
    m_clients.clear();
}

void WsTransport::send(QWebSocket *socket,
                       std::string_view type,
                       std::string_view topic,
                       std::optional<CmdId> cid,
                       std::string_view payloadJson) const
{
    if (!socket || socket->state() != QAbstractSocket::ConnectedState)
        return;

    // The envelope shape comes from the shared header; the payload is spliced as
    // text, so an event that core serialized once travels straight to the wire.
    const JsonText out = makeEnvelope(type, topic, cid, payloadJson);
    socket->sendTextMessage(QString::fromUtf8(out.data(), static_cast<qsizetype>(out.size())));
}

void WsTransport::sendProtocolError(QWebSocket *socket,
                                    std::optional<CmdId> cid,
                                    std::string_view code,
                                    std::string_view message) const
{
    send(socket, kEnvelopeTypeError, kTopicProtocolError, cid, makeProtocolErrorPayload(code, message));
}

void WsTransport::sendCmdResponse(QWebSocket *socket,
                                  CmdId cid,
                                  const QString &cmdTopic,
                                  std::string_view payloadJson) const
{
    // The only outbound path that parses: it adds `error: null` *if absent*, and
    // deciding that from raw text would be a substring guess. Command responses are
    // user-driven, so one parse here is the cheap side of the trade.
    QJsonObject out =
        QJsonDocument::fromJson(QByteArray::fromRawData(payloadJson.data(),
                                                       static_cast<qsizetype>(payloadJson.size())))
            .object();
    out.insert(QStringLiteral("cmd"), cmdTopic);
    if (!out.contains(QStringLiteral("error")))
        out.insert(QStringLiteral("error"), QJsonValue::Null);
    const QByteArray bytes = QJsonDocument(out).toJson(QJsonDocument::Compact);
    send(socket,
         kEnvelopeTypeResponse,
         kTopicCmdResponse,
         cid,
         std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
}

void WsTransport::broadcastEvent(std::string_view topic, std::string_view payloadJson) const
{
    // No cid on events; otherwise the same envelope as everything else.
    //
    // Events carry live state - channel values, adapter status - so they go only
    // to sockets that logged in. Otherwise anything that can open a connection
    // would read the house without ever authenticating, which is the same leak
    // the command gate closes (F-42).
    for (QWebSocket *client : m_clients) {
        if (m_sessions.value(client).token.isEmpty())
            continue;
        send(client, kEnvelopeTypeEvent, topic, std::nullopt, payloadJson);
    }
}

void WsTransport::handleCommand(QWebSocket *socket,
                                CmdId cid,
                                const QString &topic,
                                const QString &requestClientId,
                                const QString &requestAuthToken,
                                std::string_view payloadJson)
{
    // Routing is the protocol's decision, made once in TransportPluginBase. What
    // is left here is what only this transport knows: which client asked, and how
    // to frame the answer.
    //
    // The identity comes from the connection, not from the frame: a client cannot
    // hand itself a session by putting a token in a payload (F-42, F-60).
    const ClientSession session = m_sessions.value(socket);
    const std::string sessionToken = session.token.toStdString();
    const std::string sessionClientId = session.clientId.toStdString();
    CallerIdentity caller;
    if (!sessionToken.empty()) {
        caller.kind = CallerIdentity::Kind::Session;
        caller.sessionToken = sessionToken;
        caller.clientId = sessionClientId;
    }
    const CommandOutcome outcome = dispatchCommand(topic.toUtf8().toStdString(), payloadJson, caller);

    // A login, a bootstrap or a hello that core accepted establishes the session
    // this connection speaks with from now on.
    trackAuthOutcome(socket, topic, requestClientId, requestAuthToken, outcome.payloadJson);

    if (outcome.cmdId > 0) {
        // Core took the command and answers later; the client waits under that id
        // until onCoreAsyncResult arrives.
        PendingCommand pending;
        pending.socket = socket;
        pending.cid = cid;
        pending.cmdTopic = topic;
        m_pendingCommands.insert(outcome.cmdId, pending);
    }

    const auto [type, envelopeTopic] = envelopeFor(outcome.kind);
    send(socket, type, envelopeTopic, cid, outcome.payloadJson);
}

} // namespace phicore::transport::ws

PHI_TRANSPORT_PLUGIN(phicore::transport::ws::WsTransport)
