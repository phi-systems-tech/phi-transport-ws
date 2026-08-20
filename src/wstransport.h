#pragma once

#include <QJsonObject>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QJsonValue>

#include <optional>
#include <string>
#include <string_view>

#include <transportinterface.h>

class QHostAddress;
class QWebSocket;
class QWebSocketServer;

namespace phicore::transport::ws {

// QObject first, as Qt requires for multiple inheritance. The transport
// contract itself is Qt-free; this plugin uses Qt for its own I/O, which is its
// business rather than the contract's.
class WsTransport final : public QObject, public TransportPluginBase
{
    Q_OBJECT

public:
    explicit WsTransport(QObject *parent = nullptr);

    std::string pluginType() const override;
    std::string displayName() const override;
    std::string description() const override;

    bool start(std::string_view configJson, std::string *errorString) override;
    void stop() override;

protected:
    void onCoreAsyncResult(CmdId cmdId, std::string_view payloadJson) override;
    void onCoreEvent(std::string_view topic, std::string_view payloadJson) override;

private slots:
    void onNewConnection();
    void onSocketDisconnected();
    void onTextMessageReceived(const QString &message);

private:
    struct PendingCommand {
        QPointer<QWebSocket> socket;
        quint64 cid = 0;
        QString cmdTopic;
    };

    static bool isConfigValid(const QJsonObject &config, QString *errorString);
    static bool tryReadCid(const QJsonValue &value, quint64 *cidOut);
    static QString hostFromConfig(const QJsonObject &config);
    static quint16 portFromConfig(const QJsonObject &config);
    static QJsonObject makeAckPayload(bool accepted,
                                      const QString &cmdTopic,
                                      const QString &errorMsg = QString(),
                                      const QString &errorCode = QStringLiteral("core_error"));

    bool startServer(const QString &host, quint16 port, QString *errorString);
    void closeAllClients();
    // Outbound helpers take JSON text: nesting a payload under "payload" is pure
    // concatenation, so an event forwarded from core is never re-parsed.
    void sendEnvelope(QWebSocket *socket,
                      const QString &type,
                      const QString &topic,
                      std::optional<quint64> cid,
                      std::string_view payloadJson) const;
    void sendProtocolError(QWebSocket *socket,
                           std::optional<quint64> cid,
                           const QString &code,
                           const QString &message) const;
    void sendSyncResponse(QWebSocket *socket,
                          quint64 cid,
                          const QString &syncTopic,
                          std::string_view payloadJson) const;
    void sendAck(QWebSocket *socket,
                 quint64 cid,
                 bool accepted,
                 const QString &cmdTopic,
                 const QString &errorMsg = QString()) const;
    void sendCmdResponse(QWebSocket *socket,
                         quint64 cid,
                         const QString &cmdTopic,
                         std::string_view payloadJson) const;
    void sendEvent(QWebSocket *socket,
                   const QString &topic,
                   std::string_view payloadJson) const;
    void broadcastEvent(const QString &topic, std::string_view payloadJson) const;
    void handleCommand(QWebSocket *socket,
                       quint64 cid,
                       const QString &topic,
                       std::string_view payloadJson);

    bool m_running = false;
    QJsonObject m_config;
    QWebSocketServer *m_server = nullptr;
    QSet<QWebSocket *> m_clients;
    QHash<CmdId, PendingCommand> m_pendingCommands;
};

} // namespace phicore::transport::ws
