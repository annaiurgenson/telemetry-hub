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

Milestone 0 provides the project skeleton, strict warning configuration, application placeholders, and smoke-test infrastructure. Milestone 1 adds `BoundedBlockingQueue<T>` and a fixed-size `WorkerPool` with bounded submission, cancellation, futures, and graceful draining shutdown. Milestone 2 adds Winsock resource ownership and nonblocking IPv4-loopback TCP byte transport with stop-aware receive, complete writes, finite-timeout polling, and localhost integration tests. Milestone 3A adds transport-independent incremental binary framing. Milestone 3B adds the typed, transport-independent JSON request/response codec, strict validation, bounded parsing, and socket-free tests. Session integration, domain processing, logging, and telemetry storage remain future milestones.

Architecture, approved decisions, risks, and milestone exit criteria are recorded in [docs/PROJECT_PLAN.md](docs/PROJECT_PLAN.md). Development proceeds one explicitly approved milestone at a time.
