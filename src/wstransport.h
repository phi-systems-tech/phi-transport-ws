#pragma once

#include <transportinterface.h>

namespace phicore::transport::ws {

class WsTransport final : public TransportInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PHI_TRANSPORT_INTERFACE_IID)
    Q_INTERFACES(phicore::transport::TransportInterface)

public:
    explicit WsTransport(QObject *parent = nullptr);

    QString pluginType() const override;
    QString displayName() const override;
    QString description() const override;
    QString apiVersion() const override;

    bool start(const QJsonObject &config, QString *errorString) override;
    void stop() override;
    bool reloadConfig(const QJsonObject &config, QString *errorString) override;

private:
    static bool isConfigValid(const QJsonObject &config, QString *errorString);

    bool m_running = false;
    QJsonObject m_config;
};

} // namespace phicore::transport::ws
