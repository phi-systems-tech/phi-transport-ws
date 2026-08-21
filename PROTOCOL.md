# WebSocket Protocol Supplement (v1)

This document defines WebSocket-specific behavior for `phi-transport-ws`.
Canonical cross-transport semantics are defined in `phi-transport-api/PROTOCOLL.md`.

## Scope

- WebSocket handshake and subprotocol requirements
- Envelope validation behavior implemented by `WsTransport`
- The per-connection session gate this transport enforces before routing
- Routing behavior from wire topics to `CoreFacade`

## Handshake

- Transport: WebSocket (`ws://` / `wss://`)
- Subprotocol required by this transport: `phi-core-ws.v1`
- One WebSocket text frame must contain one JSON object envelope
- If the handshake carries an `Origin` header, it must be a loopback origin or
  listed in the transport's `allowedOrigins` config; otherwise the upgrade is
  answered with `403 Access Forbidden`. Requests without `Origin` (non-browser
  clients) are not checked.

## Envelope

Expected message shape:

```json
{
  "type": "cmd",
  "cid": 1,
  "topic": "sync.hello.get",
  "payload": {}
}
```

Validation rules:

- client messages must use `type="cmd"`
- `cid` is required and must be numeric (number or numeric string)
- `topic` is required and must be non-empty
- payload is read as object; non-object payload values are treated as `{}` by current implementation

## Session Gate

The session lives on the connection, not in the frame.

- A new connection is unauthenticated.
- While unauthenticated, only these topics are routed to core:
  - `sync.hello.get`
  - `sync.ping.get`
  - `sync.auth.*`
- Any other topic is answered with `protocol.error` code `unauthenticated` and
  is never dispatched.
- A `sync.auth.bootstrap.set`, `sync.auth.login.set` or `sync.hello.get` that
  core answers with a session token authenticates the connection.
- From then on, the transport attaches that session identity to every frame it
  forwards. A token inside a payload does not change the caller identity.
- `sync.auth.logout.set` returns the connection to the unauthenticated state.
- `event.*` and `stream.*` frames are pushed only to authenticated connections.
- An answer that establishes a session carries `sessionIdleSec`. The connection
  is closed once that many seconds pass without a frame from the client; frames
  the server pushes do not count. `sessionIdleSec` absent or `0` means core does
  not expire sessions, and neither does the transport.

## Routing

- `sync.*` topics:
  - routed via `callCoreSync`
  - success response topic: `sync.response`
- `cmd.*` topics:
  - routed only via `callCoreAsync` (no sync fallback)
  - accepted request: emit `cmd.ack` then later `cmd.response` with same `cid`
  - rejected request: emit `cmd.ack` with `accepted=false`
- unknown topic prefix:
  - emit `protocol.error` with code `unknown_topic`

## Server->Client Topics

- `sync.response`
- `cmd.ack`
- `cmd.response`
- `event.*` (forwarded core events)
- `stream.*` (forwarded core stream lifecycle/events)
- `protocol.error`

## `protocol.error` Codes (current implementation)

- `invalid_json`
- `missing_cid`
- `invalid_type`
- `missing_topic`
- `unknown_topic`
- `unauthenticated`

## Notes

- `cmd.*` uses strict async semantics in v1: `cmd.ack` (accepted/rejected), and
  for accepted commands exactly one later `cmd.response`.
