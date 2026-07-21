# TelemetryHub

TelemetryHub is an incremental C++20 portfolio project: a multithreaded Windows TCP server that receives measurements from simulated devices and returns request-correlated responses and statistics.

The project demonstrates:

- correct length-prefixed TCP stream framing and complete writes;
- UTF-8 JSON payloads with protocol versioning and bounded message sizes;
- RAII-based Winsock and socket ownership;
- bounded queues, backpressure, and a fixed worker pool;
- typed commands and separation of protocol, networking, and domain logic;
- predictable shutdown with owned `std::jthread` instances;
- focused GoogleTest unit tests and localhost integration tests;
- strict MSVC warnings and a reproducible CMake workflow.

## Target environment

- Windows 10
- Visual Studio 2022 / MSVC
- C++20
- CMake with presets
- Winsock

Planned executables are `telemetry_server` and `device_simulator`. Planned commands are `PING`, `REGISTER_DEVICE`, `SUBMIT_TELEMETRY`, and `GET_STATISTICS`.

## Protocol summary

Each frame contains a 4-byte unsigned payload length in network byte order followed by a UTF-8 JSON payload of at most 64 KiB. Requests and responses use protocol version 1 and `std::uint64_t` request IDs. Responses may arrive out of order and are correlated by request ID.

Connections receive server-generated identities distinct from registered device IDs. Devices must register before telemetry operations, and responses are always routed to the originating connection.

## Build

From a Visual Studio 2022 developer environment:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

The first configure downloads the pinned GoogleTest dependency and therefore requires network access.

## Status

Milestone 0 provides the project skeleton, strict warning configuration, application placeholders, and smoke-test infrastructure. Milestone 1 adds the reusable `BoundedBlockingQueue<T>` concurrency primitive and a fixed-size `WorkerPool` with bounded submission, cancellation, futures, and graceful draining shutdown. Winsock networking, protocol parsing, logging, and telemetry processing are not implemented yet.

Architecture, approved decisions, risks, and milestone exit criteria are recorded in [docs/PROJECT_PLAN.md](docs/PROJECT_PLAN.md). Development proceeds one explicitly approved milestone at a time.
