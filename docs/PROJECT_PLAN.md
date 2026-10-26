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
  -> ingress dispatcher
  -> bounded WorkerPool task queue / TelemetryProcessor / TelemetryStore
  -> thread-safe TcpServer::try_enqueue
  -> bounded per-connection outbound queue
  -> ClientSession writer
  -> framed TCP bytes
```

- **Platform/networking:** `WinsockRuntime`, move-only `Socket`, and `TcpServer` own platform resources through RAII.
- **Connection lifecycle:** `TcpServer` owns Winsock, the listener, and a registry keyed by server-generated `ConnectionId`. Each `ClientSession` owns one socket, one reader, one bounded outbound queue, and one coordinated writer thread. Session threads borrow the socket and never join or destroy their session.
- **Wire protocol:** `MessageFramer` reconstructs frames from arbitrary TCP chunks. Serialization and parsing remain independent of Winsock.
- **Command model:** a `std::variant` represents typed commands. `CommandEnvelope` carries request, connection, and registered-device context without owning a socket.
- **Concurrency:** bounded queues provide backpressure. A fixed worker pool processes commands; workers never call `send()`.
- **Domain:** `TelemetryProcessor` validates and executes commands. `TelemetryStore` synchronizes statistics independently of networking.
- **Responses:** workers call the thread-safe, nonblocking response sink directly; `TcpServer::try_enqueue` routes by `ConnectionId` to each bounded outbound queue. There is no global completed-response queue or dispatcher.
- **Observability:** a small dependency-free, thread-safe logger will eventually record severity, timestamp, thread ID, message, and optional `std::source_location`.

## Approved protocol and behavior

### Framing and JSON

- A frame is a 4-byte unsigned payload length in network byte order followed by a UTF-8 JSON payload.
- Every request and response includes `"version": 1`.
- The maximum payload is 64 KiB, represented by the named compile-time constant `max_payload_size_bytes`.
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

- The processing path uses a bounded global incoming-command queue, a bounded WorkerPool task queue, and one bounded outbound-message queue per connection.
- Capacities are named configuration values: incoming and worker-task queues default to 1,024 each; the worker count defaults to four.
- The global incoming-command queue is bounded (default 1,024); a nonblocking submission that finds it full returns `SERVER_BUSY` when the per-connection outbound queue can admit the response.
- Each connection outbound queue is bounded. A nonblocking enqueue that finds it full closes only that slow connection; immediate protocol errors and consumer responses use the same queue and byte limits.

## Ownership, concurrency, and shutdown

- Resource-owning handles are RAII-based and move-only where appropriate. There are no owning raw pointers or detached threads.
- The I/O model is one accept thread, one reader and writer per connection, one ingress dispatcher, and a fixed-size worker pool.
- Owned threads use `std::jthread` and `std::stop_token`. Response ordering within a connection is not guaranteed.
- Connection sockets are configured nonblocking. Per-connection I/O loops use finite-timeout WSAPoll waits and check stop tokens between waits. TcpServer owns WinsockRuntime, the listener, and a mutex-protected registry of sessions; each ClientSession owns its socket while reader and writer threads borrow the handle. The accept thread removes completed sessions under the registry lock, then joins and destroys them without holding that lock. Shutdown closes enqueue admission, stops and joins acceptance, closes the listener, closes session outbound queues, and requests checked shutdown(SD_BOTH) on active sockets. Finite-timeout polling and stop tokens remain the reliable cancellation mechanism; local socket shutdown is an additional cooperative step. I/O threads are joined before session sockets are released. Shutdown then waits for all admitted enqueue operations to finish before releasing its session owners and Winsock.
- Lock order is lifecycle_mutex_ then enqueue_mutex_ only for start/shutdown admission transitions. Enqueue admission releases enqueue_mutex_ before looking up a session under registry_mutex_; these locks are never nested. No lifecycle or registry lock is held during queue waits, thread joins, or the in-flight operation barrier wait.
- Public operations, including enqueue() and shutdown(), may overlap. The owning TcpServer must outlive every public operation; destruction must not overlap an external call. Concurrent shutdown calls are idempotent, and restart is allowed after shutdown completes.
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
|   |-- telemetryhub_tcp_integration_tests (+ GTest::gtest_main)
|   `-- telemetryhub_tcp_server_integration_tests (+ GTest::gtest_main)
|-- telemetryhub_domain
|   `-- telemetryhub_telemetry_store_tests (+ GTest::gtest_main)
|-- telemetryhub_protocol
|   |-- telemetryhub_message_framer_tests (+ GTest::gtest_main)
|   `-- telemetryhub_message_codec_tests (+ GTest::gtest_main)
|-- telemetry_server
|-- device_simulator
|-- telemetryhub_smoke_tests (+ GTest::gtest_main)
`-- telemetryhub_concurrency_tests (+ GTest::gtest_main)
```

Reusable component libraries link `telemetryhub_project_options`; executables and tests link only the components they use. `telemetryhub_protocol` contains transport-independent framing and JSON parsing/serialization; nlohmann/json is private to this target, which does not link Winsock. Third-party GoogleTest targets do not inherit project warning-as-error flags.

## Risks and controls

1. TCP has no message boundaries: `MessageFramer` handles partial prefixes, partial payloads, and combined frames with bounded buffering.
2. Bounded queues can deadlock shutdown: closure and stop-aware blocking operations are designed and tested first.
3. Slow clients can exhaust memory: outbound queues are bounded and persistent overflow closes the connection.
4. Concurrent writes can corrupt frames: each connection has exactly one coordinated writer.
5. Responses race with disconnect/reconnect: stable connection IDs, not device IDs, control routing.
6. Worker completion is unordered: request IDs provide correlation and ordering is explicitly not guaranteed.
7. `std::stop_token` cannot interrupt Winsock by itself: local `shutdown()` alone did not release a pending receive in Milestone 2C testing. Milestone 2D addressed this with finite-timeout `WSAPoll()` waits and stop-aware I/O; Milestone 4A retains this cancellation and join-before-close behavior.
8. JSON parsing has resource costs: framing and codec entry points cap payloads at 64 KiB; parser callbacks cap nesting at 16 and object members at 16; typed validation checks field and numeric constraints.
9. Fetching GoogleTest and nlohmann/json requires network access on a clean configure; both dependencies are version/hash pinned.

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
- Exit: raw localhost exchange, bounded reads, complete-write results, resource ownership, error paths, and receive interruption followed by join-before-close cleanup are demonstrated. Production session shutdown is implemented in Milestone 4A.

Milestone 2A adds only the platform resource foundation. `WinsockRuntime` is a non-copyable and non-movable lexical guard that requests Winsock 2.2 and balances each successful `WSAStartup` with one `WSACleanup`. `Socket` is a move-only owner of one `SOCKET`; explicit `close()` and `reset()` return a Winsock-category `std::error_code`, preserve ownership on close failure, and never throw from destruction. Move assignment reports failure to close its prior handle with `std::system_error`. No socket communication or application integration is included.

#### Milestone 2B - Basic TCP communication

- Establish one localhost server connection and one client connection using `bind()`, `listen()`, `accept()`, and `connect()` with explicit Winsock error handling.
- Complete a minimal `PING`/`PONG` exchange as raw bounded bytes, with helpers that handle partial writes and bounded reads without assuming TCP message boundaries.
- Add deterministic localhost integration tests for connection establishment, complete writes, bounded reads, peer closure, and expected error paths. Active receive interruption is covered by the Milestone 2D stop-aware I/O helpers.
- Keep length-prefixed framing, JSON, typed protocol messages, request IDs, telemetry processing, client-session architecture, and application composition deferred to later milestones.
- Exit: the one-server/one-client localhost exchange, bounded I/O behavior, error paths, and peer-closure tests pass repeatedly without external network access or leaked sockets/threads.

Milestone 2B uses focused free functions rather than a server or session abstraction. Setup operations throw `std::system_error` with Winsock codes and operation context. `send_all()` succeeds only after the full byte span is written and otherwise reports both the sent prefix length and error. `receive_some()` reads no more than its nonempty caller-provided buffer and distinguishes data, graceful peer closure, and error. Buffer lengths are safely limited to Winsock's `int` range per call. The Milestone 2B transport began as blocking, IPv4-loopback-only I/O without cancellation; Milestone 2D replaces the I/O path with nonblocking sockets and stop-aware polling. The complete-send loop handles partial `send()` results when Winsock returns them; tests use constrained buffers to exercise complete writes and backpressure.

#### Milestone 2C - Foundation test hardening

- Coordinate concurrency test actions with readiness signals rather than elapsed-time synchronization. Dedicated full/empty queue, worker backpressure, and synchronous shutdown checks retain a 20 ms noncompletion observation because the public APIs expose no internal wait-entry notification. Readiness immediately before an operation does not prove that its internal wait has begun.
- Test pending localhost receive behavior with peer closure and peer `shutdown(SD_SEND)`. A local-shutdown-only experiment failed its two-second completion deadline on the tested Windows installation; peer closure released the reader during failure cleanup.
- Exit: the full suite and repeated concurrency/TCP runs pass. Milestone 2D subsequently added production stop-aware polling I/O.
- These tests demonstrate peer-driven receive interruption, not reliable local-only cancellation. Milestone 2D resolved local cancellation through finite-timeout polling. Milestones 3A and 3B provide binary framing and typed JSON protocol behavior. Milestone 4A now supplies the TCP session lifecycle while continuing to use finite-timeout polling and join-before-close ownership.

#### Milestone 2D - Interruptible socket I/O foundation

- Configure stream sockets as nonblocking with a checked Winsock mode operation.
- Add finite-timeout `WSAPoll()` readiness waits (100 ms default) with distinct ready, timeout/retry, cancellation, and error outcomes.
- Add stop-aware receive and complete-send helpers. Retry `WSAEWOULDBLOCK` after polling; preserve partial-send counts on error or cancellation.
- Keep socket ownership with the caller/connection object. Borrowing I/O threads must exit and be joined before the owner closes or destroys the socket. Do not rely on local `shutdown(SD_BOTH)` for cancellation.
- Exit: localhost tests cover idle-reader cancellation, data and EOF, repeated cancellation, constrained-buffer writer cancellation, complete writes, would-block backpressure, and join-before-close ownership. No protocol or session behavior is introduced.

### Milestone 3 - Message protocol

#### Milestone 3A - Binary message framing (complete)

- Implement transport-independent incremental framing with a four-byte unsigned big-endian payload length and a named 64 KiB maximum.
- Provide complete-frame encoding and explicit errors for zero-length and oversized payloads. A fatal length error discards partial state, preserves frames completed earlier in that call, and permanently poisons the framer; later calls report `already_failed`.
- Keep internal buffering bounded to one partial payload no larger than the limit. Framing does not depend on Winsock, JSON, or application logic.
- Exit: deterministic no-socket unit tests cover fragmented prefixes and payloads, combined and incomplete frames, size boundaries, byte order, arbitrary binary preservation, and fatal error behavior.

#### Milestone 3B - JSON protocol and typed messages (complete)

- `telemetryhub_protocol` privately uses nlohmann/json v3.12.0 from the official `json.tar.xz` v3.12.0 release, pinned by SHA-256 `42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa`. JSON types do not appear in public headers.
- `MessageCodec` parses and serializes complete payloads independently of Winsock, sessions, concurrency, and domain processing. It accepts and returns payload bytes and reuses the framing layer's 64 KiB limit; it does not duplicate framing or add `ConnectionId` routing.
- Version is the JSON integer `1`. `request_id` and statistics `sample_count` are canonical decimal strings mapped to `std::uint64_t`, including the full `UINT64_MAX` range. All responses contain `version`, `request_id`, and exactly one of `result` or `error`. Every success result has a `type`; success booleans are not used.
- Request objects have exactly these fields by command: `PING` (`version`, `request_id`, `command`); `REGISTER_DEVICE` adds `device_id`; `SUBMIT_TELEMETRY` adds `metric` and finite numeric `value`; `GET_STATISTICS` adds `metric`.
- Success response result objects: `PONG` (`type`); `DEVICE_REGISTERED` (`type`, `device_id`); `TELEMETRY_ACCEPTED` (`type`); `STATISTICS` (`type`, `metric`, string `sample_count`, finite `minimum`, `maximum`, `mean`, `last_value`, and `last_received_at`). Error objects contain stable uppercase `code` and human-readable `message`. Domain processing supplies domain error codes later; the codec validates and transports them without implementing domain rules.
- Reject duplicate, unknown, missing, and wrongly typed fields; non-integer or unsupported versions; invalid or noncanonical IDs; malformed JSON, comments, trailing commas/content; invalid UTF-8; and raw NUL bytes anywhere in the provided byte span. Identifiers are nonempty, control-free UTF-8 strings no longer than 128 bytes. Error codes are nonempty uppercase ASCII letters/digits/underscores; error messages are nonempty valid UTF-8 without NUL, and `last_received_at` is an RFC 3339 UTC timestamp with millisecond precision. Parsing enforces a maximum nesting depth of 16 and at most 16 members per object through parser callbacks. Codec entry points enforce the 65,536-byte payload cap independently of the framer.
- JSON/input errors (including syntax, UTF-8, duplicate-key, and resource-limit failures) are separate from protocol validation errors and later domain errors. Validation errors preserve an optional request ID only when it is a canonical uint64 decimal value; the codec never invents correlation.
- Exit: deterministic socket-free tests cover all message variants, round-trips, uint64 boundaries, size/depth/member limits, UTF-8/NUL, duplicates, field validation, malformed JSON, finite numeric values, outgoing validation, and error correlation.

### Milestone 4 - Multiple clients

#### Milestone 4A - TCP server and connection lifecycle (complete)

- `TcpServer` owns the Winsock runtime and a nonblocking IPv4-loopback listener. One owned accept `std::jthread` uses finite-timeout `WSAPoll()` and reaps completed sessions outside registry locks.
- Each `ClientSession` owns its accepted `Socket`, one bounded queue of owned outbound byte buffers, and one reader and one writer `std::jthread`. I/O threads borrow the socket, catch all exceptions at their entry boundary, and never join or destroy the session.
- `ConnectionId` wraps `std::uint64_t`; IDs increase monotonically for the lifetime of a `TcpServer`, including restart after shutdown. Exhaustion prevents further acceptance rather than wrapping. The configured maximum active-connection count defaults to 64. Each outbound buffer is capped at 65,540 bytes by default, enough for one maximum framed protocol message; queue memory is bounded by queue capacity multiplied by that byte limit.
- Shutdown closes enqueue admission before publishing its stopping state. It joins acceptance and session I/O without holding lifecycle or registry locks, closes outbound queues to wake blocked enqueues, and waits for every admitted enqueue to release its session reference before releasing session sockets or Winsock. It also calls checked shutdown(SD_BOTH) on active session sockets; stop tokens and finite-timeout polling remain the primary reliable cancellation path. Unexpected shutdown errors retain their Winsock code for TcpServer::shutdown_error().
- shutdown() may overlap public operations, including enqueue calls and other shutdown calls. The object must outlive those calls; destruction must not overlap a public call. Calls after shutdown admission closes return EnqueueStatus::closed; already admitted calls finish as the queue closes.
- Until protocol integration, the reader uses a fixed 4 KiB receive buffer and discards received application bytes. `TcpServer::enqueue()` is the production interface for sending already encoded owned bytes; it waits for bounded queue capacity and accepts a stop token. Stopping a session cancels unsent queued buffers.
- Exit: localhost integration tests cover start/stop, concurrent clients, unique IDs, connection-limit rejection, peer-close reaping, writer delivery, idle cancellation, enqueue admission after shutdown begins, enqueue blocked by a full outbound queue, enqueue/disconnect/reaping/shutdown races, active reader/writer shutdown, peer closure before local shutdown, concurrent shutdown calls, and restart cycles.

Milestone 4B integrates per-connection framing, typed protocol handling, and device registration. Milestone 4C adds the bounded incoming-command queue and immediate overload handling; domain processing remains in Milestone 5.

#### Milestone 4B - Protocol integration and device registration (complete)

- `telemetryhub_network` stays independent of protocol/domain types. `TcpServerConfig` accepts a generic per-connection callback factory; callbacks receive a borrowed input span and return owned outbound byte buffers plus a close action. Callbacks receive no socket handles and run outside transport locks.
- The application owns the `ProtocolSessionHandler` and `DeviceRegistrationRegistry`; per-connection callback state owns its `MessageFramer` and a shared registry reference. A `ClientSession` owns callbacks and its socket/queue/threads. Disconnect notification runs exactly once after both I/O threads exit, so it cannot overlap that session's `on_bytes` callback. I/O callback exceptions are caught at the thread boundary and close only that session.
- Complete payloads are parsed with `parse_request_payload`; typed responses use `serialize_response_payload` and `encode_frame`. No duplicate JSON parser or framing implementation is introduced.
- PING returns PONG with the request ID. Registration is one-to-one by `ConnectionId`: repeated same-device registration is idempotent, a second device on one connection returns `ALREADY_REGISTERED`, and a device owned by another connection returns `DEVICE_IN_USE`. One registry mutex linearizes registrations and disconnect compare-and-erase. If reconnect races ahead of asynchronous old-session cleanup, it can briefly receive `DEVICE_IN_USE` and retry after cleanup.
- Unregistered telemetry/statistics requests receive `DEVICE_NOT_REGISTERED`; registered requests receive `PROCESSING_UNAVAILABLE`. Responses enter the existing per-session bounded outbound queue in reader order and use its single writer. No responses are routed by `device_id`.
- A correlated protocol-validation error makes at most one nonblocking queue-admission attempt. If accepted, the queue drains with a finite configurable deadline; otherwise the session closes immediately. Shutdown cancels a pending flush. This is best-effort socket delivery, not an application-level delivery guarantee. Malformed JSON, framing failures, or validation errors without a trustworthy ID close only the affected connection.
- No global incoming-command queue, full overload/backpressure policy, worker processing, telemetry storage, or response dispatcher is introduced in 4B; those remain in Milestones 4C and 5.
- Exit: unit tests verify nonblocking queue admission; localhost tests cover fragmented and combined frames, maximum request IDs, protocol commands, registration conflicts/reconnect, correlated terminal errors, malformed-client isolation, and active-session shutdown.

#### Milestone 4C - Incoming queue, backpressure, and overload handling (complete)

- `CommandEnvelope` owns the originating `ConnectionId`, request ID, device ID snapshot, and typed telemetry/statistics command. The snapshot is taken from the registration registry before queue admission; it contains no socket or session reference.
- `CommandIngress` owns one bounded queue (default 1,024) and one `std::jthread` consumer. `try_push` is nonblocking. The consumer currently returns correlated `PROCESSING_UNAVAILABLE` responses through `TcpServer::try_enqueue`, retaining the originating `ConnectionId`; it performs no domain work.
- `DeviceRegistrationRegistry::device_for_connection` returns a synchronized owned snapshot. Disconnect cleanup remains compare-and-erase by `ConnectionId`.
- A full incoming queue returns a correlated `SERVER_BUSY` with one nonblocking outbound admission attempt. A full per-connection outbound queue requests closure of that connection after releasing queue/registry locks. PING and registration remain immediate. Commands are admitted in frame order within a batch, and immediate responses are queued by the reader in that same order. Consumer responses are asynchronous and may interleave with or overtake those immediate responses; clients correlate by request ID. A closed ingress closes only the submitting session.
- Shutdown closes ingress admission, closes the queue, drains accepted envelopes through the consumer, and joins it while `TcpServer` remains alive. The application next shuts down the WorkerPool, which drains accepted tasks while the server is live, then shuts down the server and joins all session I/O. Exceptional exits after ingress startup use the same ingress-before-server order. Session callbacks retain shared ownership of ingress and registrations until the server joins them. The ingress lifecycle mutex is released before queue operations and joins; the sink runs on the consumer, must return promptly, and must not call ingress shutdown or destroy it. Concurrent submit/shutdown is supported, while destruction must not overlap an external public call.
- This milestone did not add workers, telemetry processing, or storage. The approved Milestone 5D design uses direct worker response-sink enqueue; no completed-response queue or dispatcher is added.
- Exit: unit tests cover bounded admission, full/closed results, correlated unavailable responses, drain-on-close, and concurrent submit/shutdown. Integration tests cover queue saturation in a mixed batch, `SERVER_BUSY` correlation, stale response isolation across disconnect/reconnect, registration snapshots, malformed-client isolation, and slow-client isolation when outbound capacity is exhausted.

### Milestone 5 - Telemetry processing

#### Milestone 5A - Thread-safe TelemetryStore (complete)

- `telemetryhub_domain` provides a standalone `TelemetryStore` with a `std::map` keyed by `(device_id, metric)` and one mutex. It has no TCP, Winsock, protocol, registration, ingress, or worker-pool dependency.
- Each entry stores count, minimum, maximum, online arithmetic mean, last value, last server receive time, and the receive sequence associated with the last fields. Raw samples are not retained. The latest sequence selects `last_value` and `last_received_at`; every valid sample contributes to aggregate values, and equal sequences retain the existing last fields.
- The mean uses an online update with a sign-aware calculation to avoid overflowing the difference between opposite finite extremes. A non-finite result is rejected without changing the entry. On MSVC, `long double` is not assumed to offer greater precision than `double`; mean weighting becomes approximate when counts exceed `2^53`, while the `uint64_t` sample count remains exact.
- The store rejects empty or over-128-byte key components and non-finite samples. UTF-8 validation remains at the codec boundary. Capacity defaults to 65,536 unique keys, must be positive, and can be configured at construction. At capacity, new keys return `capacity_reached`; existing keys remain updateable. No eviction is performed.
- Updates and queries use owned values under one mutex. Existing-entry updates calculate the full candidate before commit. Key allocation or map insertion exceptions leave existing state unchanged; no mutable references or locks escape the component.
- Exit: focused unit tests cover aggregate math, key and value validation, receive ordering, capacity behavior, concurrent updates, and consistent snapshots under concurrent queries.

#### Milestone 5B - TelemetryProcessor (complete)

- `telemetryhub_domain` includes a synchronous `TelemetryProcessor` that validates trusted device IDs and metric names (nonempty, at most 128 bytes), rejects non-finite samples, submits samples to `TelemetryStore`, and returns owned statistics snapshots. It has no protocol, networking, ingress, or worker-pool dependency.
- Submission outcomes distinguish accepted, invalid input, capacity reached, count overflow, and numeric failure. Statistics queries distinguish invalid input from a valid key with no statistics (`statistics_not_found`). Count overflow and numeric failure remain explicitly mapped even though the current public store API does not offer a practical way to trigger them in focused tests.
- The caller supplies server receive time and receive sequence. The store counts every successful sample and uses the greatest receive sequence to select last value/time; equal sequences preserve the existing last fields.
- The processor borrows the store, which must outlive it and every invocation/task using it. Processor calls can run concurrently without another processor mutex because store operations are synchronized. A successful submission is returned only after the synchronized update completes. Queries return the store snapshot at their execution point; concurrent requests are not forced into receive order. A query made after observing a successful submission result sees that committed sample when it queries the same store and device/metric key.
- Protocol error conversion, response dispatch, receive metadata propagation, and processing integration are implemented by Milestone 5C; WorkerPool execution is integrated in 5D.

#### Milestone 5C - End-to-end telemetry processing (complete)

- `CommandEnvelope` owns the registered device snapshot, typed command, receive time, and receive sequence. The session handler records `system_clock::now()` after parsing a valid request and before registration lookup or ingress admission. `CommandIngress` atomically allocates a nonzero sequence starting at one before each admission attempt; rejected/full attempts may leave gaps. `UINT64_MAX` is usable once, after which ingress returns `sequence_exhausted` without wrapping and the session emits `PROCESSING_UNAVAILABLE`.
- Worker tasks call `TelemetryProcessor`, map successful submissions to `TELEMETRY_ACCEPTED`, and serialize complete statistics snapshots. Timestamps use UTC RFC 3339 format with millisecond precision, truncating sub-millisecond time. Invalid processing input maps to `INVALID_FIELD_VALUE`; missing statistics map to `STATISTICS_NOT_FOUND`; capacity, count overflow, numeric failure, and sequence exhaustion map to `PROCESSING_UNAVAILABLE`. `SERVER_BUSY` is reserved for incoming queue saturation.
- A command-level failure before calling the response sink receives at most one generic `PROCESSING_UNAVAILABLE` fallback attempt. Once sink invocation begins, the command is never retried because the sink may already have admitted the response. Per-command failures do not terminate draining of later accepted work. Ingress drains and joins before server shutdown, including exceptional application exit.
- `telemetry_server` owns store, processor, and ingress in dependency order. Existing end-to-end routing continues to use the originating `ConnectionId`; each connection's writer remains the sole socket writer. Multiple workers may complete commands out of order. Receive sequence selects the last-value fields independently of processing completion order; queries observe state when their worker executes.
- Exit: unit tests cover wire response mapping, timestamp boundaries and truncation, queue-full gaps, command-failure handling, and drain semantics. Localhost tests cover end-to-end submission/statistics, registration requirements, `STATISTICS_NOT_FOUND`, mixed-batch saturation, reconnect isolation, multi-client device isolation, and ingress drain while the server remains live.

#### Milestone 5D - WorkerPool integration (complete)

- `CommandIngress` owns one bounded incoming queue (default 1,024) and one dispatcher thread. The application composes a fixed four-thread `WorkerPool` with a bounded 1,024 task queue. The dispatcher transfers each owned envelope to the pool; bounded blocking admission propagates pressure back to the incoming queue. Once that queue is full, new submissions receive `SERVER_BUSY`.
- Maximum accepted in-flight work at the defaults is 2,053 commands: 1,024 in ingress, 1,024 in the worker queue, four active workers, and at most one command held by the dispatcher during blocked admission. No unbounded task or future collection is created.
- Worker tasks own their envelope and a copy of the thread-safe response sink. `TcpServer::try_enqueue` is nonblocking and routes by `ConnectionId` into the bounded session queue. Workers never write sockets; the session writer remains the sole sender. Responses may complete out of order and retain their request IDs. Failed or throwing response admission is handled without retrying an ambiguous response.
- The synchronized store uses receive sequence, rather than worker completion order, to select last value/time; all successful samples contribute to aggregates. Statistics queries observe the state at worker execution. An acknowledgment is built only after its store update commits and is then admitted to the outbound queue.
- Shutdown closes ingress admission and joins its dispatcher, then closes and joins the WorkerPool to drain accepted tasks while the server and response sink remain alive, then shuts down sessions. Store and processor lifetimes extend past all tasks. Normal and exceptional application exits use this order.
- There is no global completed-response queue or response dispatcher. The `AGENTS.md` rule now allows direct response-sink enqueue when it is thread-safe and nonblocking, session queues are bounded, failed admission is handled, and workers finish before the sink/server are destroyed.
- Exit: unit tests cover concurrent worker execution, response correlation and out-of-order completion, saturation/backpressure, sink failure, and draining. Localhost tests cover `SERVER_BUSY` after bounded queue saturation and orderly ingress/pool/server shutdown.

Each future milestone requires explicit approval before work begins.
### Milestone 6 - Production-quality improvements

- Add structured logging, configuration, broader integration tests, static analysis, supported sanitizers, CI, and architecture/protocol documentation.
- Exit: a clean checkout has a documented reproducible workflow enforced by CI.

#### Milestone 6A - Clean Windows CI (approved)

- Add one GitHub Actions job on the `windows-2022` hosted runner, triggered by pushes, pull requests, and manual dispatch. Grant only `contents: read`, and pin `actions/checkout` to an immutable commit with credential persistence disabled.
- Configure the existing Visual Studio 2022 x64 preset from the runner's clean checkout, build all default targets with the repository's existing strict MSVC warning policy, and execute the complete CTest suite. Keep the existing pinned `FetchContent` dependencies and local development presets unchanged.
- Before focused repeats, inspect CTest's registered names and fail if any required focused test is missing, including `CommandIngress.ShutdownDrainsWhenDispatcherIsBlockedByFullWorkerPool`. Require a nonempty selection, then repeat the explicit WorkerPool concurrency/backpressure/shutdown, CommandIngress admission/saturation/shutdown, TcpServer lifecycle/shutdown, and ProtocolSession overload/reconnect/draining cases 20 times with failure output enabled.
- Document the clean local preset workflow, full suite, focused repeat command, and distinction between local results and GitHub-hosted CI in the README.
- Acceptance criteria: workflow YAML and action pin are inspected; all configured presets and focused CTest names exist; native configure, build, and test command failures fail their steps; full and repeated focused CTest runs both pass on GitHub's Windows runner. Review the complete diff and require `git diff --check` to pass before committing.
- Verification guarantee: a passing hosted run demonstrates that this workflow passed for that commit on the `windows-2022` image. Local verification and workflow inspection do not establish that a GitHub-hosted run passed.
- Known limitations: the workflow relies on GitHub's hosted Windows image and network access for the first `FetchContent` download; this milestone does not add dependency caching or an offline build path. The workflow is not verified on GitHub until it is pushed and a hosted run completes successfully.

## Deferred implementation choices

These remain intentionally deferred until their implementing milestone:

- Capacities outside the telemetry incoming and worker-task queues, and exact full-queue wait thresholds.
- Receive-sequence generation and propagation through ingress, deferred to the response-processing milestone.
- Socket timeouts and the exact slow-client disconnect threshold.
- Logger output destination and formatting.
