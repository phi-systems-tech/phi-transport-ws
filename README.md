# phi-transport-ws

## Overview

WebSocket transport plugin for `phi-core`, based on `phi-transport-api`.

## Supported Protocols / Endpoints

- WebSocket transport (`ws`)
- Server-side endpoint (MVP implementation)

## Network Exposure

- Target is LAN endpoint for `phi-ui` and compatible clients.
- Final bind host/port and TLS policy are configured via plugin config.

## Authentication & Security

- Authentication is validated by `phi-core`.
- Transport plugin forwards auth-relevant payloads to core APIs.
- TLS handling is planned as part of runtime implementation.

## Known Issues

- MVP scope only:
  - No `event.*` forwarding yet (push events from core are still pending in WS transport).
  - No `stream.*` lifecycle emission yet (current discovery returns snapshot payloads).

## License

See `LICENSE`.

---

## Developer Documentation

### Purpose

Provide the WebSocket transport layer while keeping `phi-core` as the single API/auth authority.

### Features

- Qt plugin implementing `phicore::transport::TransportInterface`
- Dedicated WebSocket server with configurable `host`/`port`
- JSON envelope parsing (`type/topic/cid/payload`)
- ACK + async result correlation (`cmdId -> socket/cid/cmdTopic`)
- Lifecycle hooks: `start`, `stop`, `reloadConfig`

### Runtime Model

- One plugin instance per transport plugin type (`ws`).
- Intended to run in dedicated transport thread managed by `TransportManager`.

### Core Integration Contract

- Use only `callCoreSync` and `callCoreAsync` for core communication.
- No direct access to internal core registries/managers.

### Protocol Contract

Routing rule in `WsTransport`:

- `sync.*` topics are treated as synchronous and routed via `callCoreSync`.
- `cmd.*` topics are treated as commands:
  - first `callCoreAsync` (ACK + later `cmd.response` when accepted),
  - fallback to `callCoreSync` for command topics currently implemented as sync-style in core facade.
- any other prefix is rejected with `protocol.error` (`unknown_topic`).

Currently implemented command topics (via `TransportCoreFacade`):

- Sync (`sync.*`)
  - `sync.hello.get`
  - `sync.ping.get`
  - `sync.auth.bootstrap.set`
  - `sync.auth.login.set`
  - `sync.auth.logout.set`
  - `sync.settings.get`
  - `sync.settings.set`
  - `sync.settings.user.get`
  - `sync.settings.user.set`
  - `sync.users.enabled.set`
  - `sync.users.flags.set`
  - `sync.users.delete.set`
  - `sync.tr.get`
  - `sync.tr.set`
- Cmd handled async (`cmd.ack` + later `cmd.response`)
  - `cmd.channel.invoke`
  - `cmd.device.effect.invoke`
  - `cmd.scene.invoke`
  - `cmd.adapter.action.invoke`
  - `cmd.adapter.create`
  - `cmd.adapter.restart`
  - `cmd.adapter.reload`
  - `cmd.adapter.start`
  - `cmd.adapter.stop`
- Cmd currently handled sync-style in core facade (accepted via `cmd.*` prefix; immediate `cmd.response`)
  - `cmd.users.list`
  - `cmd.adapters.list`
  - `cmd.devices.list`
  - `cmd.rooms.list`
  - `cmd.groups.list`
  - `cmd.scenes.list`
  - `cmd.adapters.factories.list`
  - `cmd.room.get`
  - `cmd.group.get`
  - `cmd.room.create`
  - `cmd.group.create`
  - `cmd.scene.create`
  - `cmd.scene.scope.assign`
  - `cmd.device.group.set`
  - `cmd.automations.list`
  - `cmd.automation.create`
  - `cmd.automation.update`
  - `cmd.automation.delete`
  - `cmd.automation.run`
  - `cmd.cron.job.list`
  - `cmd.cron.job.create`
  - `cmd.cron.job.update`
  - `cmd.cron.job.delete`
  - `cmd.device.user.update`
  - `cmd.channel.user.update`
  - `cmd.adapters.discover`
  - `cmd.adapters.discoverAll`
  - `cmd.adapter.config.layout.get`
  - `cmd.adapter.action.layout.get`
  - `cmd.adapter.update`
  - `cmd.adapter.delete`

Compatibility aliases currently accepted in `TransportCoreFacade` (legacy command spelling):

- `cmd.settings.get`
- `cmd.settings.set`
- `cmd.settings.user.get`
- `cmd.settings.user.set`
- `cmd.users.enabled.set`
- `cmd.users.flags.set`
- `cmd.users.delete.set`
- `cmd.tr.get`
- `cmd.tr.set`

Response topics used by this plugin:

- `sync.response`
- `cmd.ack`
- `cmd.response`
- `protocol.error`

### Runtime Requirements

- `phi-core` with transport plugin loading enabled.
- `phi-transport-api` compatible with interface IID `tech.phi-systems.phi-core.TransportInterface/1.0`.

### Build Requirements

- CMake 3.21+
- Qt 6 Core + WebSockets
- C++20 compiler

### Configuration

Minimal config example:

```json
{
  "host": "0.0.0.0",
  "port": 5022
}
```

Current validation:
- `port` optional; if present must be `1..65535`.

### Build

```bash
cmake -S . -B build
cmake --build build --parallel
```

Resolution order for `phi-transport-api`:
1. `find_package(phi-transport-api CONFIG)`
2. sibling checkout fallback: `../phi-transport-api`

### Installation

```bash
cmake --install build
```

- Output module: `libphi_transport_ws.so`
- Default install destination: `lib/phi/plugins/transports`

### Observability

- Logging categories and metrics are planned with runtime implementation.

### Troubleshooting

- CMake cannot find `phi-transport-api`:
  - Install `phi-transport-api-dev` or place repo at `../phi-transport-api`.
- Plugin does not load:
  - Verify IID and deployment path under transport plugin directory.

### Maintainers

- Phi Systems Tech team

### Issue Tracker

- `https://github.com/phi-systems-tech/phi-transport-ws/issues`

### Releases / Changelog

- Releases: `https://github.com/phi-systems-tech/phi-transport-ws/releases`
- Tags: `https://github.com/phi-systems-tech/phi-transport-ws/tags`
