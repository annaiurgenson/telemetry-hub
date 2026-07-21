# TelemetryHub Project Plan

## Purpose

TelemetryHub is a portfolio-scale C++20 system demonstrating robust TCP stream handling, bounded concurrency, explicit ownership, testable domain logic, and graceful shutdown. Simulated devices register, submit measurements, request statistics, and receive request-correlated responses.

Development is incremental. Every milestone must leave the repository buildable and tested; work advances only with explicit approval.

## Architecture

```text
TCP bytes
  -> ClientSession reader
  -> MessageFramer / JSON parser
  -> CommandEnvelope
  -> bounded incoming queue
  -> WorkerPool / TelemetryProcessor / TelemetryStore
  -> bounded completed-response queue
  -> ResponseDispatcher
  -> bounded per-connection outbound queue
  -> ClientSession writer
  -> framed TCP bytes
```

- **Platform/networking:** `WinsockRuntime`, move-only `Socket`, and `TcpServer` own platform resources through RAII.
- **Connection lifecycle:** each TCP connection receives a server-generated `ConnectionId`, one reader thread, one bounded outbound queue, and one coordinated writer thread.
- **Wire protocol:** `MessageFramer` reconstructs frames from arbitrary TCP chunks. Serialization and parsing remain independent of Winsock.
- **Command model:** a `std::variant` represents typed commands. `CommandEnvelope` carries request, connection, and registered-device context without owning a socket.
- **Concurrency:** bounded queues provide backpressure. A fixed worker pool processes commands; workers never call `send()`.
- **Domain:** `TelemetryProcessor` validates and executes commands. `TelemetryStore` synchronizes statistics independently of networking.
- **Responses:** an optional global completed-response queue and single dispatcher route responses by `ConnectionId` to per-connection outbound queues.
- **Observability:** a small dependency-free, thread-safe logger will eventually record severity, timestamp, thread ID, message, and optional `std::source_location`.

## Approved protocol and behavior

### Framing and JSON

- A frame is a 4-byte unsigned payload length in network byte order followed by a UTF-8 JSON payload.
- Every request and response includes `"version": 1`.
- The initial maximum payload is 64 KiB, represented by one named configurable constant.
- Request IDs use `std::uint64_t`. Every response includes the corresponding `request_id`.
- Responses may complete and arrive out of order; clients correlate them using `request_id`.
- Protocol errors contain a stable error code and a human-readable message.
- A valid frame with an unsupported or invalid command receives a structured error when possible.
- A malformed frame, invalid length, or oversized payload closes only that connection and must not terminate the server.

### Connection and device identity

- `ConnectionId` identifies one specific TCP connection and is separate from `device_id`.
- A connection must register a device before submitting telemetry or requesting statistics.
- Registration is rejected while that `device_id` belongs to another active connection.
- A device may reconnect after its prior connection closes.
- Responses are routed only by the originating `ConnectionId`; an old response is never delivered to a newer connection for the same device.

### Telemetry model

- Each submission has a metric name and one numeric value. Device identity comes from the registered connection.
- Statistics are maintained per `(device_id, metric)` pair: sample count, minimum, maximum, arithmetic mean, last value, and last server receive time.
- The initial version does not retain raw sample history.

### Backpressure and overload

- The processing path uses a bounded global incoming-command queue, a bounded completed-response queue when used, and one bounded outbound-message queue per connection.
- Capacities are named configuration values; defaults will be proposed in the implementing milestone.
- When the incoming queue is full, the server returns `SERVER_BUSY` when safe.
- A persistently full outbound queue identifies a slow client; the server closes that connection instead of growing memory without bound.

## Ownership, concurrency, and shutdown

- Resource-owning handles are RAII-based and move-only where appropriate. There are no owning raw pointers or detached threads.
- The initial I/O model is one accept thread, one reader and writer per connection, a fixed-size worker pool, and one dispatcher thread if the global completed-response queue is used.
- Owned threads use `std::jthread` and `std::stop_token`. Response ordering within a connection is not guaranteed.
- Shutdown stops acceptance, requests stop, calls Winsock `shutdown()` on active sockets to unblock `recv()` and `send()`, closes queues according to their drain/cancel policy, joins threads, and finally releases sockets and Winsock.
- Startup or unrecoverable configuration failures may throw exceptions such as `std::system_error`.
- Expected connection-level network errors close only the affected connection. Protocol validation normally produces typed errors rather than cross-layer exceptions.
- Every thread-entry boundary catches all exceptions; none may escape a thread function.

## Repository structure

```text
TelemetryHub/
|-- AGENTS.md
|-- CMakeLists.txt
|-- CMakePresets.json
|-- README.md
|-- cmake/
|   `-- CompilerWarnings.cmake
|-- docs/
|   `-- PROJECT_PLAN.md
|-- include/telemetryhub/
|   |-- concurrency/
|   |-- domain/
|   |-- network/
|   |-- protocol/
|   `-- support/
|-- src/
|   |-- concurrency/
|   |-- domain/
|   |-- network/
|   |-- protocol/
|   `-- support/
|-- apps/
|   |-- telemetry_server/
|   `-- device_simulator/
`-- tests/
    |-- unit/
    `-- integration/
```

Public headers live under `include/telemetryhub`; private implementation stays in `src`. Application entry points perform composition and configuration. Unit and localhost integration tests remain separable.

## CMake target model

```text
telemetryhub_project_options (INTERFACE: C++20 and project warnings)
|-- telemetryhub_concurrency
|   `-- telemetryhub_worker_pool_tests (+ GTest::gtest_main)
|-- telemetryhub_network (privately links ws2_32)
|   |-- telemetryhub_winsock_runtime_tests (+ GTest::gtest_main)
|   |-- telemetryhub_socket_tests (+ GTest::gtest_main)
|   `-- telemetryhub_tcp_integration_tests (+ GTest::gtest_main)
|-- telemetry_server
|-- device_simulator
|-- telemetryhub_smoke_tests (+ GTest::gtest_main)
`-- telemetryhub_concurrency_tests (+ GTest::gtest_main)
```

Future reusable component libraries will link `telemetryhub_project_options`; executables and tests will link only the components they use. Third-party GoogleTest targets do not inherit project warning-as-error flags.

## Risks and controls

1. TCP has no message boundaries: incremental framing must handle partial and combined reads.
2. Bounded queues can deadlock shutdown: closure and stop-aware blocking operations are designed and tested first.
3. Slow clients can exhaust memory: outbound queues are bounded and persistent overflow closes the connection.
4. Concurrent writes can corrupt frames: each connection has exactly one coordinated writer.
5. Responses race with disconnect/reconnect: stable connection IDs, not device IDs, control routing.
6. Worker completion is unordered: request IDs provide correlation and ordering is explicitly not guaranteed.
7. `std::stop_token` cannot interrupt Winsock by itself: socket `shutdown()` participates in cancellation.
8. JSON validation has resource costs: frame size, nesting, field types, and numeric ranges require limits in Milestone 3.
9. Fetching GoogleTest requires network access on a clean configure: its version is pinned for repeatability.

## Milestones and exit criteria

### Milestone 0 - Project foundation

- Add repository layout, CMake configuration and presets, strict MSVC warnings, documentation, and pinned GoogleTest/CTest smoke infrastructure.
- Exit: configure, build, and smoke test succeed in Visual Studio 2022 with zero project warnings.

### Milestone 1 - Concurrency foundation

- Implement and test `BoundedBlockingQueue<T>` closure, capacity, backpressure, cancellation, and a fixed worker pool. No TCP.
- Exit: deterministic tests pass repeatedly without hangs or leaked joinable threads.

Milestone 1A implements only `BoundedBlockingQueue<T>`. Its operations report `success`, `closed`, or `cancelled`; close is idempotent, rejects new pushes, wakes all waiters, and allows queued values to drain. The queue uses one mutex and stop-aware `std::condition_variable_any` waits with separate not-empty and not-full conditions. Callers must stop all participating threads before destroying the queue.

Milestone 1B adds a fixed-size general-purpose `WorkerPool`. It stores move-only `std::packaged_task<void()>` work in the bounded queue and returns each submitted callable's typed future. The queue's close state is the single acceptance boundary: submissions linearized before close are accepted and drained, while blocked or later submissions report `closed`; a blocked submit may instead report `cancelled` through its caller-provided stop token. Graceful shutdown closes the queue, lets workers finish all accepted tasks, and joins every worker. A shutdown mutex serializes shutdown callers and protects the transition to the fully stopped state. Shutdown and destruction must be initiated outside the pool's own worker threads because a thread cannot join itself. Immediate cancellation of accepted tasks remains deferred.

### Milestone 2 - Basic TCP communication

- Add Winsock RAII, a move-only socket, and one-server/one-client `PING`/`PONG` communication.
- Exit: complete local reads/writes, error paths, and predictable shutdown are demonstrated.

Milestone 2A adds only the platform resource foundation. `WinsockRuntime` is a non-copyable and non-movable lexical guard that requests Winsock 2.2 and balances each successful `WSAStartup` with one `WSACleanup`. `Socket` is a move-only owner of one `SOCKET`; explicit `close()` and `reset()` return a Winsock-category `std::error_code`, preserve ownership on close failure, and never throw from destruction. Move assignment reports failure to close its prior handle with `std::system_error`. No socket communication or application integration is included.

#### Milestone 2B - Basic TCP communication

- Establish one localhost server connection and one client connection using `bind()`, `listen()`, `accept()`, and `connect()` with explicit Winsock error handling.
- Complete a minimal `PING`/`PONG` exchange as raw bounded bytes, with helpers that handle partial writes and bounded reads without assuming TCP message boundaries.
- Add deterministic localhost integration tests for connection establishment, complete writes, bounded reads, peer closure, expected error paths, and predictable shutdown.
- Keep length-prefixed framing, JSON, typed protocol messages, request IDs, telemetry processing, worker-pool integration, client-session architecture, and application composition deferred to later milestones.
- Exit: the one-server/one-client localhost exchange, complete I/O behavior, error paths, and shutdown tests pass repeatedly without external network access or leaked sockets/threads.

Milestone 2B uses focused free functions rather than a server or session abstraction. Setup operations throw `std::system_error` with Winsock codes and operation context. `send_all()` succeeds only after the full byte span is written and otherwise reports both the sent prefix length and error. `receive_some()` reads no more than its nonempty caller-provided buffer and distinguishes data, graceful peer closure, and error. Buffer lengths are safely limited to Winsock's `int` range per call. This transport is blocking, IPv4-loopback-only, and has no cancellation or production shutdown coordination. Deterministically forcing a partial `send()` is deferred; the write loop handles it when Winsock returns one naturally.

### Milestone 3 - Message protocol

- Implement the approved length-prefixed JSON protocol, request IDs, typed parsing/serialization, validation, and size limits.
- Exit: protocol tests cover partial/combined frames, malformed data, oversize frames, UTF-8, and byte order without real sockets.

### Milestone 4 - Multiple clients

- Add sessions, the connection registry, incoming queue, per-client I/O threads and outbound queues, and overload/disconnect handling.
- Exit: integration tests exercise concurrent clients, registration conflicts, reconnects, malformed clients, and shutdown.

### Milestone 5 - Telemetry processing

- Add telemetry submission, approved statistics, worker processing, thread-safe storage, the response path, and dispatcher.
- Exit: unit and integration tests verify concurrent statistics and prevent cross-connection response delivery.

### Milestone 6 - Production-quality improvements

- Add structured logging, configuration, broader integration tests, static analysis, supported sanitizers, CI, and architecture/protocol documentation.
- Exit: a clean checkout has a documented reproducible workflow enforced by CI.

## Deferred implementation choices

These remain intentionally deferred until their implementing milestone:

- Default capacities and exact full-queue wait thresholds.
- Worker-count default and shutdown drain-versus-cancel policy for each queue.
- Concrete JSON library and detailed command/response field schema.
- Numeric acceptance rules and arithmetic-mean precision/overflow strategy.
- Socket timeouts and the exact slow-client disconnect threshold.
- Logger output destination and formatting.
