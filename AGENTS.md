# TelemetryHub Development Rules

## Scope and workflow

- Work on one user-approved milestone at a time. Do not begin the next milestone without explicit approval.
- Before implementation, explain the design and list the files to create or modify.
- Keep changes small and reviewable. Afterward, explain key decisions, tests, warnings, and known limitations.
- Build and run applicable tests after every meaningful implementation step.
- Do not add speculative abstractions or features outside the active milestone.

## Technical baseline

- Target Windows 10, Visual Studio 2022, MSVC, C++20, CMake, and Winsock.
- Keep networking, framing/serialization, command parsing, concurrency, domain processing, storage, and logging separate.
- Use RAII and explicit ownership. Do not use owning raw pointers or detached threads.
- Prefer the rule of zero; make resource-owning types move-only where appropriate.
- Use bounded queues and bounded network messages. Handle partial TCP reads and writes.
- Use controlled cancellation and a documented shutdown order. No exception may escape a thread entry function.
- Check system-call results and preserve useful error context.
- Use modern C++ features when they simplify the design; avoid cleverness that harms readability.
- Enable strict warnings. Fix warnings; suppress one only with a documented, narrow reason.
- Use UTF-8 JSON protocol payloads framed by a 4-byte network-order length. Protocol version 1 and the 64 KiB payload limit are named constants when implemented.
- Use `std::uint64_t` request IDs. Responses may arrive out of order and must retain both the request ID and the originating server-generated connection ID.
- Keep `ConnectionId` separate from `device_id`; never route a response by device ID.
- Require registration before telemetry submission or statistics queries. A device ID may have only one active connection.
- Use a bounded incoming queue and bounded per-connection outbound queues. A completed-response queue is optional; workers may enqueue directly through a thread-safe, nonblocking response sink when session queues are bounded, failed admission is handled, and workers finish before the sink/server are destroyed. Workers never write sockets directly; each connection has one writer.
- Use one accept thread, one reader and writer per active connection, a fixed worker pool, and an optional single response dispatcher. Own them with `std::jthread`.
- During shutdown, request cooperative stop and call `shutdown()` on active sockets to unblock Winsock operations.
- Treat expected connection failures locally. Use exceptions for startup/unrecoverable configuration failures and catch all exceptions at thread boundaries.

## Quality bar

- Add focused tests for normal behavior, boundaries, failure paths, cancellation, and shutdown.
- Keep tests deterministic; use timeouts only as safety bounds, not as synchronization.
- Avoid hidden global mutable state and ambiguous lifetime relationships.
- Document protocol limits, concurrency guarantees, and overload behavior as they are introduced.
- Never claim a build or test passed unless it was actually run; report environment blockers honestly.
- Use GoogleTest pinned through CMake `FetchContent`, with tests registered in CTest.
- Keep the initial logger small, thread-safe, and dependency-free; defer it until its milestone unless foundation code requires it.
