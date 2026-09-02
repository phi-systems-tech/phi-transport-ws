#pragma once

#include "wsserver.h"

#include <transportinterface.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace phicore::transport::ws {

// The WS transport on the runtime loop (contract 2.0.0): WsServer speaks
// RFC 6455 over Loop watches, this class speaks the phi protocol over it.
// No Qt and no thread of its own - the loop's thread is where everything
// here runs, including the contract callbacks.
class WsTransport final : public TransportPluginBase
{
public:
    WsTransport() = default;
    ~WsTransport() override;

    std::string pluginType() const override;
    std::string displayName() const override;
    std::string description() const override;

    bool start(std::string_view configJson, std::string *errorString) override;
    void stop() override;

protected:
    void onCoreAsyncResult(CmdId cmdId, std::string_view payloadJson) override;
    void onCoreEvent(std::string_view topic, std::string_view payloadJson) override;

private:
    using Json = nlohmann::json;
    using ConnId = WsServer::ConnId;

    struct PendingCommand {
        ConnId conn = 0;
        std::uint64_t cid = 0;
        std::string cmdTopic;
    };

    // What one connection has established. A connection starts
    // unauthenticated and may only reach the pre-auth topics until it logs in
    // (F-42); after that the identity comes from here rather than from
    // whatever a frame claims.
    struct ClientSession {
        std::string token;
        std::string clientId;
        // The clock this connection is judged by: core states the budget when
        // it hands out the session, and every authorized frame resets it.
        std::int64_t idleBudgetMs = 0;
        std::int64_t lastActivityMs = 0;
    };

    static bool isConfigValid(const Json &config, std::string *errorString);
    static std::string hostFromConfig(const Json &config);
    static std::uint16_t portFromConfig(const Json &config);
    // Which JSON shapes a cid may arrive in; what counts as a valid one is the
    // protocol's answer and lives in the shared header.
    static std::optional<CmdId> readCid(const Json &value);
    static std::vector<std::string> allowedOriginsFromConfig(const Json &config);
    static bool isLoopbackOrigin(const std::string &origin);
    /// True when a connection that has not authenticated may send this topic.
    static bool isPreAuthTopic(std::string_view topic);

    bool acceptOrigin(const std::string &origin);
    void onConnected(ConnId id, const std::string &peerAddress, std::uint16_t peerPort);
    void onDisconnected(ConnId id);
    void onTextMessage(ConnId id, std::string_view message);

    /// Closes the connections whose session has sat idle past its budget.
    void dropIdleSessions();
    /// Reads a session out of an auth response and remembers or forgets it.
    void trackAuthOutcome(ConnId id,
                          const std::string &topic,
                          const std::string &requestClientId,
                          const std::string &requestAuthToken,
                          std::string_view responsePayloadJson);

    // The one outbound primitive. Envelope and payload shapes come from
    // envelope.h, so this only puts assembled text on a connection.
    void send(ConnId id,
              std::string_view type,
              std::string_view topic,
              std::optional<CmdId> cid,
              std::string_view payloadJson);
    void sendProtocolError(ConnId id, std::optional<CmdId> cid,
                           std::string_view code, std::string_view message);
    void sendCmdResponse(ConnId id, CmdId cid,
                         const std::string &cmdTopic, std::string_view payloadJson);
    void broadcastEvent(std::string_view topic, std::string_view payloadJson);
    void handleCommand(ConnId id,
                       CmdId cid,
                       const std::string &topic,
                       const std::string &requestClientId,
                       const std::string &requestAuthToken,
                       std::string_view payloadJson);

    WsServer m_server;
    bool m_running = false;
    std::vector<std::string> m_allowedOrigins;
    phi::runtime::Timer m_idleSweep;
    std::map<ConnId, ClientSession> m_sessions;
    std::map<CmdId, PendingCommand> m_pendingCommands; // key: core cmdId
    std::int64_t m_lastStatsLogMs = 0;
    std::uint64_t m_eventsSinceLast = 0;
    std::uint64_t m_channelEventsSinceLast = 0;
};

} // namespace phicore::transport::ws
