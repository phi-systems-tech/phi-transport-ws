# phi-transport-ws

## Overview

WebSocket transport plugin for `phi-core`, based on `phi-transport-api`.

## Supported Protocols / Endpoints

- WebSocket transport (`ws`)
- Server-side endpoint (MVP implementation)

## Network Exposure

- Target is LAN endpoint for `phi-ui` and compatible clients.
- Bind host/port come from the transport config passed in by `phi-core`.
- `phi-core` resolves that transport config in two layers:
  - `/etc/phi/transports/ws.json` as the default base config
  - `/var/lib/phi/transports/ws/current/config.json` as the runtime override
- The `phi-transport-ws` Debian package provides `/etc/phi/transports/ws.json`
  with the default localhost binding.

## Authentication & Security

- Authentication is validated by `phi-core`.
- Transport plugin forwards auth-relevant payloads to core APIs.
- TLS handling is planned as part of runtime implementation.

## Known Issues

- MVP scope only:
  - Stream kinds beyond `adapter.discover` are not implemented yet.

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
- Lifecycle hooks: `start`, `stop`

### Runtime Model

- One plugin instance per transport plugin type (`ws`).
- Intended to run in dedicated transport thread managed by `TransportManager`.

### Core Integration Contract

- Use only `callCoreSync` and `callCoreAsync` for core communication.
- No direct access to internal core registries/managers.

### Protocol Contract

- Canonical cross-transport contract: `phi-transport-api/PROTOCOLL.md`
- WebSocket-specific supplement for this plugin: `PROTOCOL.md`
- `WsTransport` routes `sync.*` via `callCoreSync`.
- `WsTransport` routes `cmd.*` only via `callCoreAsync` (strict v1, no sync fallback).
- Wire responses used by this plugin: `sync.response`, `cmd.ack`, `cmd.response`,
  `event.*`, `stream.*`, `protocol.error`.

### Runtime Requirements

- `phi-core` with transport plugin loading enabled.
- `phi-transport-api` compatible with interface IID `tech.phi-systems.phi-core.TransportInterface/1.0`.

### Build Requirements

- CMake 3.21+
- Qt 6 Core + WebSockets
- C++20 compiler

### Configuration

- Runtime config is passed in by `phi-core`.
- Transport lifecycle commands are owned by `phi-core`:
  - `restart` = stop/start with freshly resolved config
  - `reload` = unload/load plugin binary, then start with freshly resolved config

Minimal config example:

```json
{
  "host": "127.0.0.1",
  "port": 5040
}
```

Current validation:
- `host` optional; defaults to `127.0.0.1` when omitted.
- `port` optional; defaults to `5040` when omitted.
- Default package config path: `/etc/phi/transports/ws.json`
- Runtime override path: `/var/lib/phi/transports/ws/current/config.json`

### Build

```bash
cmake -S . -B ../build/phi-transport-ws/release-ninja -G Ninja
cmake --build ../build/phi-transport-ws/release-ninja --parallel
```

Resolution order for `phi-transport-api`:
1. `find_package(phi-transport-api CONFIG)`
2. sibling checkout fallback: `../phi-transport-api`

### Installation

```bash
cmake --install ../build/phi-transport-ws/release-ninja
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
