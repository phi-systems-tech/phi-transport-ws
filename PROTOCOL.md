# WebSocket Protocol Supplement (v1)

This document defines WebSocket-specific behavior for `phi-transport-ws`.
Canonical cross-transport semantics are defined in `phi-transport-api/PROTOCOLL.md`.

## Scope

- WebSocket handshake and subprotocol requirements
- Envelope validation behavior implemented by `WsTransport`
- Routing behavior from wire topics to `CoreFacade`

## Handshake

- Transport: WebSocket (`ws://` / `wss://`)
- Subprotocol required by this transport: `phi-core-ws.v1`
- One WebSocket text frame must contain one JSON object envelope

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

## Notes

- `cmd.*` uses strict async semantics in v1: `cmd.ack` (accepted/rejected), and
  for accepted commands exactly one later `cmd.response`.
