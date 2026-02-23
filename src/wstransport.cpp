#include "wstransport.h"

#include <QHostAddress>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLoggingCategory>
#include <QWebSocket>
#include <QWebSocketServer>

Q_LOGGING_CATEGORY(wsTransportLog, "phi-transport.ws")

namespace phicore::transport::ws {

namespace {

constexpr quint16 kDefaultPort = 5042;
constexpr const char kTypeEvent[] = "event";
constexpr const char kTypeCmd[] = "cmd";
constexpr const char kTypeResponse[] = "response";
constexpr const char kTypeError[] = "error";

constexpr const char kTopicCmdAck[] = "cmd.ack";
constexpr const char kTopicCmdResponse[] = "cmd.response";
constexpr const char kTopicSyncResponse[] = "sync.response";
constexpr const char kTopicProtocolError[] = "protocol.error";

} // namespace

WsTransport::WsTransport(QObject *parent)
    : TransportInterface(parent)
{
}

QString WsTransport::pluginType() const
{
    return QStringLiteral("ws");
}

QString WsTransport::displayName() const
{
    return QStringLiteral("WebSocket");
}

QString WsTransport::description() const
{
    return QStringLiteral("WebSocket transport plugin for phi-core APIs.");
}

QString WsTransport::apiVersion() const
{
    return QStringLiteral("1.0.0");
}

bool WsTransport::start(const QJsonObject &config, QString *errorString)
{
    if (!isConfigValid(config, errorString))
        return false;

    if (m_running)
        stop();

    const QString host = hostFromConfig(config);
    const quint16 port = portFromConfig(config);
    if (!startServer(host, port, errorString))
        return false;

    m_config = config;
    m_running = true;
    qCInfo(wsTransportLog).noquote()
        << tr("WS transport started on %1:%2").arg(host).arg(port);
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

bool WsTransport::reloadConfig(const QJsonObject &config, QString *errorString)
{
    if (!isConfigValid(config, errorString))
        return false;

    if (!m_running) {
        m_config = config;
        return true;
    }

    stop();
    return start(config, errorString);
}

void WsTransport::onCoreAsyncResult(CmdId cmdId, const QJsonObject &payload)
{
    auto it = m_pendingCommands.find(cmdId);
    if (it == m_pendingCommands.end())
        return;

    const PendingCommand pending = it.value();
    m_pendingCommands.erase(it);

    QWebSocket *socket = pending.socket.data();
    if (!socket || socket->state() != QAbstractSocket::ConnectedState)
        return;

    sendCmdResponse(socket, pending.cid, pending.cmdTopic, payload);
}

void WsTransport::onCoreEvent(const QString &topic, const QJsonObject &payload)
{
    if (topic.trimmed().isEmpty())
        return;
    static qint64 s_lastStatsLogMs = 0;
    static quint64 s_eventsSinceLast = 0;
    static quint64 s_channelEventsSinceLast = 0;
    ++s_eventsSinceLast;
    if (topic == QStringLiteral("event.channel.stateChanged"))
        ++s_channelEventsSinceLast;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (s_lastStatsLogMs <= 0 || (nowMs - s_lastStatsLogMs) >= 5000) {
        qCInfo(wsTransportLog).noquote()
            << QStringLiteral("WS broadcast stats: clients=%1 events=%2 channelEvents=%3")
                   .arg(m_clients.size())
                   .arg(static_cast<qulonglong>(s_eventsSinceLast))
                   .arg(static_cast<qulonglong>(s_channelEventsSinceLast));
        s_eventsSinceLast = 0;
        s_channelEventsSinceLast = 0;
        s_lastStatsLogMs = nowMs;
    }
    broadcastEvent(topic, payload);
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
        qCInfo(wsTransportLog).noquote()
            << QStringLiteral("WS client connected: %1:%2 total=%3")
                   .arg(socket->peerAddress().toString())
                   .arg(socket->peerPort())
                   .arg(m_clients.size());
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
    qCInfo(wsTransportLog).noquote()
        << QStringLiteral("WS client disconnected: %1:%2 total=%3")
               .arg(socket->peerAddress().toString())
               .arg(socket->peerPort())
               .arg(m_clients.size());

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
        sendProtocolError(socket, std::nullopt, QStringLiteral("invalid_json"),
                          QStringLiteral("Payload must be a valid JSON object."));
        return;
    }

    const QJsonObject obj = doc.object();
    const QString type = obj.value(QStringLiteral("type")).toString();
    const QString topic = obj.value(QStringLiteral("topic")).toString();
    const QJsonValue cidValue = obj.value(QStringLiteral("cid"));
    const QJsonObject payload = obj.value(QStringLiteral("payload")).toObject();

    quint64 cid = 0;
    if (!tryReadCid(cidValue, &cid)) {
        sendProtocolError(socket, std::nullopt, QStringLiteral("missing_cid"),
                          QStringLiteral("Commands must include numeric 'cid'."));
        return;
    }

    if (type != QLatin1String(kTypeCmd)) {
        sendProtocolError(socket, cid, QStringLiteral("invalid_type"),
                          QStringLiteral("Only messages with type='cmd' are supported."));
        return;
    }

    if (topic.trimmed().isEmpty()) {
        sendProtocolError(socket, cid, QStringLiteral("missing_topic"),
                          QStringLiteral("Missing command topic."));
        return;
    }

    handleCommand(socket, cid, topic, payload);
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

bool WsTransport::tryReadCid(const QJsonValue &value, quint64 *cidOut)
{
    if (!cidOut)
        return false;

    if (value.isDouble()) {
        const double raw = value.toDouble(-1.0);
        if (raw < 0.0)
            return false;
        *cidOut = static_cast<quint64>(raw);
        return true;
    }

    if (value.isString()) {
        bool ok = false;
        const quint64 parsed = value.toString().toULongLong(&ok);
        if (!ok)
            return false;
        *cidOut = parsed;
        return true;
    }

    return false;
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

QJsonObject WsTransport::makeAckPayload(bool accepted,
                                        const QString &cmdTopic,
                                        const QString &errorMsg,
                                        const QString &errorCode)
{
    QJsonObject payload;
    payload.insert(QStringLiteral("accepted"), accepted);
    payload.insert(QStringLiteral("cmd"), cmdTopic);
    if (errorMsg.isEmpty()) {
        payload.insert(QStringLiteral("error"), QJsonValue::Null);
    } else {
        QJsonObject err;
        err.insert(QStringLiteral("code"), errorCode);
        err.insert(QStringLiteral("msg"), errorMsg);
        payload.insert(QStringLiteral("error"), err);
    }
    return payload;
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

void WsTransport::sendEnvelope(QWebSocket *socket,
                               const QString &type,
                               const QString &topic,
                               quint64 cid,
                               const QJsonObject &payload) const
{
    if (!socket || socket->state() != QAbstractSocket::ConnectedState)
        return;

    QJsonObject env;
    env.insert(QStringLiteral("type"), type);
    env.insert(QStringLiteral("topic"), topic);
    env.insert(QStringLiteral("cid"), static_cast<qint64>(cid));
    env.insert(QStringLiteral("payload"), payload);
    socket->sendTextMessage(QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact)));
}

void WsTransport::sendProtocolError(QWebSocket *socket,
                                    std::optional<quint64> cid,
                                    const QString &code,
                                    const QString &message) const
{
    if (!socket || socket->state() != QAbstractSocket::ConnectedState)
        return;

    QJsonObject payload;
    payload.insert(QStringLiteral("code"), code);
    payload.insert(QStringLiteral("message"), message);

    QJsonObject env;
    env.insert(QStringLiteral("type"), QString::fromLatin1(kTypeError));
    env.insert(QStringLiteral("topic"), QString::fromLatin1(kTopicProtocolError));
    if (cid.has_value())
        env.insert(QStringLiteral("cid"), static_cast<qint64>(*cid));
    env.insert(QStringLiteral("payload"), payload);
    socket->sendTextMessage(QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact)));
}

void WsTransport::sendSyncResponse(QWebSocket *socket,
                                   quint64 cid,
                                   const QString &syncTopic,
                                   const QJsonObject &payload) const
{
    QJsonObject out = payload;
    out.insert(QStringLiteral("sync"), syncTopic);
    sendEnvelope(socket, QString::fromLatin1(kTypeResponse), QString::fromLatin1(kTopicSyncResponse), cid, out);
}

void WsTransport::sendAck(QWebSocket *socket,
                          quint64 cid,
                          bool accepted,
                          const QString &cmdTopic,
                          const QString &errorMsg) const
{
    const QJsonObject payload = makeAckPayload(accepted, cmdTopic, errorMsg);
    sendEnvelope(socket, QString::fromLatin1(kTypeResponse), QString::fromLatin1(kTopicCmdAck), cid, payload);
}

void WsTransport::sendCmdResponse(QWebSocket *socket,
                                  quint64 cid,
                                  const QString &cmdTopic,
                                  const QJsonObject &payload) const
{
    QJsonObject out = payload;
    out.insert(QStringLiteral("cmd"), cmdTopic);
    if (!out.contains(QStringLiteral("error")))
        out.insert(QStringLiteral("error"), QJsonValue::Null);
    sendEnvelope(socket, QString::fromLatin1(kTypeResponse), QString::fromLatin1(kTopicCmdResponse), cid, out);
}

void WsTransport::sendEvent(QWebSocket *socket,
                            const QString &topic,
                            const QJsonObject &payload) const
{
    if (!socket || socket->state() != QAbstractSocket::ConnectedState)
        return;

    QJsonObject env;
    env.insert(QStringLiteral("type"), QString::fromLatin1(kTypeEvent));
    env.insert(QStringLiteral("topic"), topic);
    env.insert(QStringLiteral("payload"), payload);
    socket->sendTextMessage(QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact)));
}

void WsTransport::broadcastEvent(const QString &topic, const QJsonObject &payload) const
{
    for (QWebSocket *client : m_clients)
        sendEvent(client, topic, payload);
}

void WsTransport::handleCommand(QWebSocket *socket,
                                quint64 cid,
                                const QString &topic,
                                const QJsonObject &payload)
{
    if (topic.startsWith(QStringLiteral("sync."))) {
        const SyncResult result = callCoreSync(topic, payload);
        if (result.accepted) {
            sendSyncResponse(socket, cid, topic, result.payload);
        } else {
            const QString err = result.error.has_value() ? result.error->msg : QStringLiteral("Sync call rejected");
            QJsonObject out;
            out.insert(QStringLiteral("accepted"), false);
            QJsonObject errObj;
            errObj.insert(QStringLiteral("msg"), err);
            if (result.error.has_value() && !result.error->ctx.isEmpty())
                errObj.insert(QStringLiteral("ctx"), result.error->ctx);
            out.insert(QStringLiteral("error"), errObj);
            sendSyncResponse(socket, cid, topic, out);
        }
        return;
    }

    if (!topic.startsWith(QStringLiteral("cmd."))) {
        sendProtocolError(socket, cid, QStringLiteral("unknown_topic"),
                          QStringLiteral("Unknown command topic: %1").arg(topic));
        return;
    }

    const AsyncResult asyncSubmit = callCoreAsync(topic, payload);
    if (asyncSubmit.accepted && asyncSubmit.cmdId > 0) {
        PendingCommand pending;
        pending.socket = socket;
        pending.cid = cid;
        pending.cmdTopic = topic;
        m_pendingCommands.insert(asyncSubmit.cmdId, pending);
        sendAck(socket, cid, true, topic);
        return;
    }

    const SyncResult syncResult = callCoreSync(topic, payload);
    if (syncResult.accepted) {
        sendAck(socket, cid, true, topic);
        sendCmdResponse(socket, cid, topic, syncResult.payload);
        return;
    }

    const bool unknownTopic =
        asyncSubmit.error.has_value()
        && syncResult.error.has_value()
        && asyncSubmit.error->msg == QStringLiteral("Unsupported async topic")
        && syncResult.error->msg == QStringLiteral("Unsupported sync topic");

    if (unknownTopic) {
        sendProtocolError(socket, cid, QStringLiteral("unknown_topic"),
                          QStringLiteral("Unknown command topic: %1").arg(topic));
        return;
    }

    const QString errorMsg =
        syncResult.error.has_value() && !syncResult.error->msg.isEmpty()
            ? syncResult.error->msg
            : (asyncSubmit.error.has_value() && !asyncSubmit.error->msg.isEmpty()
                   ? asyncSubmit.error->msg
                   : QStringLiteral("Command rejected"));
    sendAck(socket, cid, false, topic, errorMsg);
}

} // namespace phicore::transport::ws
