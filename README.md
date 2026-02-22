# phi-transport-ws

## Overview

WebSocket transport plugin for `phi-core`, based on `phi-transport-api`.

## Supported Protocols / Endpoints

- WebSocket transport (`ws`)
- Server-side endpoint (runtime implementation pending)

## Network Exposure

- Target is LAN endpoint for `phi-ui` and compatible clients.
- Final bind host/port and TLS policy are configured via plugin config.

## Authentication & Security

- Authentication is validated by `phi-core`.
- Transport plugin forwards auth-relevant payloads to core APIs.
- TLS handling is planned as part of runtime implementation.

## Known Issues

- Current state is a compile-verified skeleton without production WS runtime.

## License

See `LICENSE`.

---

## Developer Documentation

### Purpose

Provide the WebSocket transport layer while keeping `phi-core` as the single API/auth authority.

### Features

- Qt plugin implementing `phicore::transport::TransportInterface`
- Lifecycle hooks: `start`, `stop`, `reloadConfig`
- Config validation for `port`

### Runtime Model

- One plugin instance per transport plugin type (`ws`).
- Intended to run in dedicated transport thread managed by `TransportManager`.

### Core Integration Contract

- Use only `callCoreSync` and `callCoreAsync` for core communication.
- No direct access to internal core registries/managers.

### Protocol Contract

- Pending implementation for command envelope and ACK/result flow mapping.

### Runtime Requirements

- `phi-core` with transport plugin loading enabled.
- `phi-transport-api` compatible with interface IID `tech.phi-systems.phi-core.TransportInterface/1.0`.

### Build Requirements

- CMake 3.21+
- Qt 6 Core
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
