#include "wstransport.h"

namespace phicore::transport::ws {

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

    m_config = config;
    m_running = true;
    return true;
}

void WsTransport::stop()
{
    m_running = false;
}

bool WsTransport::reloadConfig(const QJsonObject &config, QString *errorString)
{
    if (!isConfigValid(config, errorString))
        return false;

    m_config = config;
    return true;
}

bool WsTransport::isConfigValid(const QJsonObject &config, QString *errorString)
{
    const auto portValue = config.value(QStringLiteral("port"));
    if (portValue.isUndefined())
        return true;

    const int port = portValue.toInt(-1);
    if (port < 1 || port > 65535) {
        if (errorString)
            *errorString = QStringLiteral("Invalid 'port' value; expected 1..65535.");
        return false;
    }

    return true;
}

} // namespace phicore::transport::ws
