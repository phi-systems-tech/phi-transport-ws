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
  - `/etc/phi/@1/transports/ws.json` as the default base config
  - `/var/lib/phi/@1/transports/ws/current/config.json` as the runtime override
- The `phi-transport-ws` Debian package provides `/etc/phi/@1/transports/ws.json`
  with the default localhost binding.
- Binding to a non-loopback address makes the endpoint reachable from the LAN
  (and, behind a forwarding router, from the WAN). Everything behind it is then
  protected by the login this plugin enforces per connection — see
  Authentication & Security for the origin allowlist and the TLS statement.

## Authentication & Security

- This transport owns the client-facing auth boundary. `phi-core` trusts the
  caller identity this plugin attaches to every frame, so an unauthenticated
  socket must never reach a privileged topic.
- Every connection carries its own session. Before a successful login only the
  pre-auth topics are forwarded to core:
  - `sync.hello.get`
  - `sync.ping.get`
  - `sync.auth.*` (bootstrap, login, logout)
- Any other topic on an unauthenticated connection is refused by the transport
  itself with error code `unauthenticated`; the frame is never dispatched.
- The session token is taken from the connection, not from the frame. A client
  logs in once per socket; later frames need no token in their payload, and a
  token in the payload cannot raise another connection's privileges.
- Browser origins are checked at the WebSocket handshake, which is exempt from
  the same-origin policy: without this check any web page a LAN user visits
  could open a socket to this endpoint. Non-browser clients send no `Origin`
  header and are unaffected.
  - Default allowlist: loopback origins only (`http(s)://localhost[:port]`,
    `http(s)://127.0.0.1[:port]`, `http(s)://[::1][:port]`).
  - Serving `phi-ui` from another host requires listing its origin explicitly
    in `allowedOrigins` (see Configuration). A refused handshake is answered
    with `403 Access Forbidden` and logged in the `security` category.
- Sessions expire. Core states how long a session may sit without a single call
  (`sessionIdleSec`, from the `security.sessionIdleSec` setting) in the answer
  that hands out the token, and this transport closes the connection once that
  budget passes without a call from the client. What counts is what core counts:
  a topic it authorizes, which is where it touches the session. Server pushes do
  not count, and neither do the pre-auth topics - a heartbeat says the socket is
  open, not that anyone is still using it.
- Events are pushed only to authenticated connections. Channel values and
  adapter status are live state; a socket that never logged in sees nothing.
- Login throttling, password hashing and capability checks live in `phi-core`;
  this plugin does not cache credentials and stores no password material.
- TLS is **not** terminated by this plugin. Exposing the endpoint beyond the
  local host means putting a TLS-terminating reverse proxy in front of it —
  otherwise session tokens travel in clear text.

## Known Issues

- MVP scope only:
  - Stream start/stop is generic via `cmd.stream.start|stop`, with semantic
    selection via `kind` (for example `adapter.discover`, `network.discover`,
    `raw.discover`).
  - Stream kinds beyond `adapter.discover` are not implemented yet.

## License

See `LICENSE`.

---

## Developer Documentation

### Purpose

Provide the WebSocket transport layer while keeping `phi-core` as the single API/auth authority.

### Features

- Shared-object plugin implementing `phicore::transport::TransportInterface` (Qt-free contract; the plugin uses Qt internally)
- Dedicated WebSocket server with configurable `host`/`port`
- JSON envelope parsing (`type/topic/cid/payload`)
- ACK + async result correlation (`cmdId -> socket/cid/cmdTopic`)
- Lifecycle hooks: `start`, `stop`

### Runtime Model

- One plugin instance per transport plugin type (`ws`).
- Runs on the core-owned transport thread and uses its event loop (PROTOCOLL.md 6.6):
  the `QWebSocketServer` and every client socket live there. This plugin starts no
  thread and no event loop of its own.

### Core Integration Contract

- Use only `callCoreSync` and `callCoreAsync` for core communication.
- No direct access to internal core registries/managers.

### Protocol Contract

- Canonical cross-transport contract: `phi-transport-api/PROTOCOLL.md`
- WebSocket-specific supplement for this plugin: `PROTOCOL.md`
- `WsTransport` routes `sync.*` via `callCoreSync`.
- Calls into core carry a `CallerIdentity`: the session token from the frame being handled, or
  `Anonymous` when the client sent none, in which case core allows only the pre-auth topics. Once
  this transport authenticates its own connections, the token should come from the connection's
  state instead - core will not notice the difference, which is why the identity is a parameter.
- `cmd.*` is routed only via `callCoreAsync` (strict v1, no sync fallback). The rule itself
  lives in `TransportPluginBase::dispatchCommand` in `phi-transport-api`, together with the
  envelope and ack shapes, so every transport answers a topic the same way.
- Wire responses used by this plugin: `sync.response`, `cmd.ack`, `cmd.response`,
  `event.*`, `stream.*`, `protocol.error`.

### Runtime Requirements

- `phi-core` with transport plugin loading enabled.
- `phi-transport-api` whose `kTransportApiVersion` matches what phi-core expects; the plugin exports it as `phi_transport_api_version`.

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
  "port": 5040,
  "allowedOrigins": ["http://phi-box.local:5022"]
}
```

Current validation:
- `host` optional; defaults to `127.0.0.1` when omitted.
- `port` optional; defaults to `5040` when omitted.
- `allowedOrigins` optional array of strings; each entry is one exact origin
  (scheme, host and — if non-default — port), compared case-insensitively.
  When omitted, only loopback origins are accepted. Entries are additive: the
  loopback defaults stay valid. `"*"` is not supported on purpose.
- Default package config path: `/etc/phi/@1/transports/ws.json`
- Runtime override path: `/var/lib/phi/@1/transports/ws/current/config.json`

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

- Transport runtime logs are emitted as structured `LogEntry` records via the
  shared `phi-core` logging pipeline.
- Do not use Qt logging categories as a parallel transport log path.
- Metrics remain a separate concern and are still planned.

### Troubleshooting

- CMake cannot find `phi-transport-api`:
  - Install `phi-transport-api-dev` or place repo at `../phi-transport-api`.
- Plugin does not load:
  - Verify the reported API version and the deployment path under the transport plugin directory.

### Maintainers

- Phi Systems Tech team

### Issue Tracker

- `https://github.com/phi-systems-tech/phi-transport-ws/issues`

### Releases / Changelog

- Releases: `https://github.com/phi-systems-tech/phi-transport-ws/releases`
- Tags: `https://github.com/phi-systems-tech/phi-transport-ws/tags`
