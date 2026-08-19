#pragma once

#include <QHash>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QJsonValue>

#include <optional>
#include <string_view>

#include <transportinterface.h>

class QHostAddress;
class QWebSocket;
class QWebSocketServer;

namespace phicore::transport::ws {

class WsTransport final : public TransportPluginBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PHI_TRANSPORT_INTERFACE_IID)
    Q_INTERFACES(phicore::transport::TransportInterface)

public:
    explicit WsTransport(QObject *parent = nullptr);

    QString pluginType() const override;
    QString displayName() const override;
    QString description() const override;

    bool start(std::string_view configJson, QString *errorString) override;
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
