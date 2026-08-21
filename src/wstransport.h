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
    static QString hostFromConfig(const QJsonObject &config);
    static quint16 portFromConfig(const QJsonObject &config);
    // Which JSON shapes a cid may arrive in; what counts as a valid one is the
    // protocol's answer and lives in the shared header.
    static std::optional<CmdId> readCid(const QJsonValue &value);

    bool startServer(const QString &host, quint16 port, QString *errorString);
    void closeAllClients();
    // The one outbound primitive. Envelope and payload shapes come from
    // envelope.h, so this only puts assembled text on a socket.
    void send(QWebSocket *socket,
              std::string_view type,
              std::string_view topic,
              std::optional<CmdId> cid,
              std::string_view payloadJson) const;
    void sendProtocolError(QWebSocket *socket,
                           std::optional<CmdId> cid,
                           std::string_view code,
                           std::string_view message) const;
    void sendCmdResponse(QWebSocket *socket,
                         CmdId cid,
                         const QString &cmdTopic,
                         std::string_view payloadJson) const;
    void broadcastEvent(std::string_view topic, std::string_view payloadJson) const;
    void handleCommand(QWebSocket *socket,
                       CmdId cid,
                       const QString &topic,
                       std::string_view payloadJson);

    // Read off the frame currently being handled; see onTextMessageReceived.
    QString m_pendingToken;
    QString m_pendingClientId;
    bool m_running = false;
    QJsonObject m_config;
    QWebSocketServer *m_server = nullptr;
    QSet<QWebSocket *> m_clients;
    QHash<CmdId, PendingCommand> m_pendingCommands;
};

} // namespace phicore::transport::ws
