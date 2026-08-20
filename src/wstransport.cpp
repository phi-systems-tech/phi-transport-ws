#include "wstransport.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QHostAddress>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QWebSocket>
#include <QWebSocketServer>

namespace phicore::transport::ws {

namespace {

// Envelope types and the topics a transport produces itself now come from
// envelope.h - they are protocol surface, and two transports owning a copy each
// is how the wire drifts.
constexpr quint16 kDefaultPort = 5040;

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

    closeAllClients();
    m_clients.clear();
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

    // The API takes the payload as text; this transport parsed the frame to read the
    // envelope, so the sub-object is serialized once here. That extra step is the
    // cost side of the text boundary, and it sits on the command path rather than on
    // the event path.
    const QByteArray payloadBytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    handleCommand(socket,
                  *cid,
                  topic,
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

    connect(server, &QWebSocketServer::newConnection,
            this, &WsTransport::onNewConnection);

    m_server = server;
    return true;
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
    for (QWebSocket *client : m_clients)
        send(client, kEnvelopeTypeEvent, topic, std::nullopt, payloadJson);
}

void WsTransport::handleCommand(QWebSocket *socket,
                                CmdId cid,
                                const QString &topic,
                                std::string_view payloadJson)
{
    // Routing is the protocol's decision, made once in TransportPluginBase. What
    // is left here is what only this transport knows: which client asked, and how
    // to frame the answer.
    const CommandOutcome outcome = dispatchCommand(topic.toUtf8().toStdString(), payloadJson);

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
