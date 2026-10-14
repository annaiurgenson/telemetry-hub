# TelemetryHub

TelemetryHub is an incremental C++20 portfolio project: a multithreaded Windows TCP server that receives measurements from simulated devices and returns request-correlated responses and statistics.

The project demonstrates:

- length-prefixed TCP stream framing and complete writes;
- a bounded UTF-8 JSON protocol with typed messages and versioning;
- RAII-based Winsock and socket ownership;
- bounded queues, backpressure, and a fixed worker pool;
- separation of protocol, networking, and domain logic;
- predictable shutdown with owned `std::jthread` instances;
- focused GoogleTest unit tests and localhost integration tests;
- strict MSVC warnings and a reproducible CMake workflow.

## Target environment

- Windows 10
- Visual Studio 2022 / MSVC
- C++20
- CMake with presets
- Winsock

Planned executables are `telemetry_server` and `device_simulator`.

## Protocol summary

Each TCP frame contains a 4-byte unsigned payload length in network byte order followed by a UTF-8 JSON payload. The payload must contain 1–65,536 bytes. Requests and responses use integer protocol version `1`. Request IDs are canonical decimal strings on the wire and `std::uint64_t` in C++; this preserves values through `18446744073709551615` without JSON/JavaScript number precision loss. Responses may arrive out of order and are correlated by request ID.

Request shapes (fields shown are required):

```json
{"version":1,"request_id":"9007199254740993","command":"PING"}
{"version":1,"request_id":"42","command":"REGISTER_DEVICE","device_id":"sensor-17"}
{"version":1,"request_id":"43","command":"SUBMIT_TELEMETRY","metric":"temperature","value":23.75}
{"version":1,"request_id":"44","command":"GET_STATISTICS","metric":"temperature"}
```

Successful responses use `result.type`; every response has exactly one of `result` or `error`:

```json
{"version":1,"request_id":"9007199254740993","result":{"type":"PONG"}}
{"version":1,"request_id":"42","result":{"type":"DEVICE_REGISTERED","device_id":"sensor-17"}}
{"version":1,"request_id":"43","result":{"type":"TELEMETRY_ACCEPTED"}}
{"version":1,"request_id":"44","result":{"type":"STATISTICS","metric":"temperature","sample_count":"12","minimum":19.5,"maximum":24.0,"mean":21.7,"last_value":23.75,"last_received_at":"2026-10-11T12:30:00.000Z"}}
{"version":1,"request_id":"45","error":{"code":"DEVICE_NOT_REGISTERED","message":"Register a device before submitting telemetry"}}
```

The codec rejects unknown, missing, duplicate, and wrongly typed fields; unsupported versions; noncanonical or overflowing IDs; invalid UTF-8; raw NUL bytes anywhere in the payload; non-finite telemetry values; and malformed JSON, comments, trailing commas, or trailing content. Identifiers are nonempty, control-free UTF-8 strings of at most 128 bytes. Error codes use nonempty uppercase ASCII letters, digits, or underscores; error messages are nonempty UTF-8 strings without NUL, and `last_received_at` is an RFC 3339 UTC timestamp with millisecond precision. Parsing is limited to 16 nested arrays/objects and 16 members per object. `sample_count` is also a canonical decimal string. JSON syntax/resource errors are distinct from protocol validation errors; domain failures are represented later as structured error responses and are not inferred by the codec.

Connection IDs are server-generated and remain separate from device and request IDs. Devices must register before telemetry operations, and later session code routes responses by connection ID.

## Build

From a Visual Studio 2022 developer environment:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

The first configure fetches the pinned GoogleTest and nlohmann/json dependencies and therefore requires network access. nlohmann/json v3.12.0 is privately linked to `telemetryhub_protocol`; it does not appear in public headers.

## Status

Milestones 0–3B provide the project foundation, bounded concurrency primitives, nonblocking TCP byte transport, incremental framing, and the typed JSON codec. Milestone 4A adds the multi-client TCP lifecycle; Milestone 4B integrates framing and typed requests per connection and registers devices by `ConnectionId`. Milestone 4C adds a bounded incoming command queue (default capacity 1,024), nonblocking admission, `SERVER_BUSY` on saturation, and an owned draining consumer that currently returns `PROCESSING_UNAVAILABLE`. Milestone 5A adds the standalone, thread-safe `TelemetryStore` for bounded per-device/per-metric aggregates. Milestone 5B adds synchronous domain processing; protocol response mapping and metadata propagation remain in 5C, and WorkerPool integration remains in 5D.

Architecture, approved decisions, risks, and milestone exit criteria are recorded in [docs/PROJECT_PLAN.md](docs/PROJECT_PLAN.md). Development proceeds one explicitly approved milestone at a time.

## Milestone 4A connection lifecycle

`TcpServer` owns its `WinsockRuntime`, nonblocking loopback listener, accept thread, and connection registry. Each registered `ClientSession` owns its accepted socket and bounded outbound queue. Its reader and writer borrow that socket; the writer is the only sender. The accept thread reaps completed sessions after removing them from the registry lock, and joins both I/O threads before releasing the session socket.

The default active-connection limit is 64, the default outbound queue capacity is 16 owned byte buffers, and each buffer is capped at 65,540 bytes, enough for one maximum size framed protocol message. These limits bound queued memory per connection. `TcpServer::enqueue()` waits for queue capacity and accepts a caller stop token; queue closure wakes blocked enqueues during session shutdown. Session stop cancels unsent queued buffers; queue closure does not promise to drain a connection during shutdown. `ConnectionId` is a server-generated `std::uint64_t` wrapper, increases monotonically across restarts of one server object, and never wraps.

Shutdown first closes enqueue admission, then stops and joins acceptance. It closes each session queue and requests shutdown(SD_BOTH) while the session still owns its connected socket; finite-timeout WSAPoll and stop tokens remain the reliable cancellation mechanism. After joining each reader and writer, shutdown waits for all admitted enqueue calls to leave before releasing session sockets and Winsock. Unexpected socket-shutdown errors retain their Winsock error code through TcpServer::shutdown_error(); peer-disconnected and already-shut-down errors are treated as expected.

Public operations may overlap, including enqueue() and shutdown(). The TcpServer object must outlive every public call; its destructor must not overlap external calls. During this milestone, reader bytes are received into a fixed 4 KiB buffer and discarded; protocol parsing and telemetry behavior are deferred to later milestones.

## Milestone 4B protocol sessions

`telemetry_server` composes `TcpServer` with a protocol callback factory. Networking exposes only connection IDs, borrowed byte spans, and owned outbound byte frames; it has no protocol or domain dependency. Each connection callback owns an incremental `MessageFramer`, parses complete payloads with the typed JSON codec, and serializes responses through that codec and `encode_frame`.

PING returns a correlated PONG. Registration binds one device to one active connection; same-connection repeats are idempotent, while second-device and competing-owner requests return `ALREADY_REGISTERED` and `DEVICE_IN_USE`. Disconnect cleanup is performed once after both I/O threads exit and compares the disconnecting `ConnectionId` before erasing. If a peer reconnects before the old disconnect is detected and cleaned up, its registration attempt may receive `DEVICE_IN_USE`; it can retry after cleanup. Telemetry and statistics commands return `DEVICE_NOT_REGISTERED` or `PROCESSING_UNAVAILABLE` as appropriate.

Responses enter the existing bounded per-session queue in reader order and are sent by its single writer. Correlated protocol-validation errors receive one nonblocking queue-admission attempt and close after a bounded flush; if admission fails, the session closes immediately. The flush deadline is configurable and server shutdown cancels it. Socket send completion does not guarantee application-level receipt. Uncorrelatable malformed input closes only that session. No global incoming queue, worker processing, telemetry storage, or response dispatcher is implemented in 4B.
## Milestone 4C incoming queue and overload behavior

`CommandIngress` owns a bounded queue with a default capacity of 1,024 command envelopes and one consumer thread. Admission is nonblocking: a full incoming queue returns a correlated `SERVER_BUSY` through one nonblocking attempt on the originating connection's outbound queue. Accepted telemetry/statistics commands currently drain as correlated `PROCESSING_UNAVAILABLE`; domain command processing remains in Milestones 5B–5D, while the standalone store was added in 5A. Consumer replies can interleave with immediate PING and registration replies, so clients correlate responses by request ID.

Each envelope owns its `ConnectionId`, request ID, registered `device_id` snapshot, and typed command. Replies route only by the originating `ConnectionId`. If a per-connection outbound queue fills, only that slow connection is closed. Shutdown closes ingress admission at queue closure, drains accepted commands and joins the consumer while `TcpServer` remains alive, then shuts down sessions. The application preserves this order on normal and exceptional exits. The response sink must return promptly and must not shut down or destroy its ingress object.

Unit and localhost integration tests cover queue saturation and draining, producer/shutdown races, correlated unavailable and busy responses, stale-response isolation after reconnect, outbound saturation, and slow-client isolation. The 5B processor is implemented as a standalone domain component. Protocol response mapping, metadata propagation, and completed-response dispatch remain for 5C; `WorkerPool` integration remains for 5D.

## Milestone 5B domain processing

`TelemetryProcessor` borrows a `TelemetryStore` and synchronously submits telemetry or queries an owned statistics snapshot. It accepts caller-supplied receive time and sequence; sequence selects last-value and timestamp fields, while every successful sample contributes to aggregates. Invalid identifiers (empty or over 128 bytes) and non-finite values return `invalid_input`. Queries distinguish invalid input from valid keys with no statistics (`statistics_not_found`); submissions also retain distinct capacity, count-overflow, and numeric-failure outcomes.

The store must outlive the processor and all active calls. Calls can be concurrent because the store synchronizes updates and queries. A successful submission result follows the committed update. Queries observe a snapshot at their execution point; no general receive-order execution is promised. A query issued after a successful submission result sees that update for the same store and key. Protocol error conversion, response routing, ingress metadata propagation, and command processing integration are deferred to Milestone 5C; WorkerPool integration is deferred to 5D.
