# TelemetryHub — MVP Release Scope Freeze

**Decision date:** 2026-11-08  
**Goal:** Finish and publish a credible, runnable C++20 portfolio project without expanding an already strong core architecture.

## Authority and workflow

- This document records the **release scope decision**, not approval to begin every remaining milestone.
- Implement **one explicitly user-approved milestone at a time**; follow `AGENTS.md` for design review, changes, build/test, diff review, and commits.
- The latest explicitly approved work has priority over earlier tentative milestone names. In particular, **Milestone 6C is Minimal Runtime Configuration, NOT a device simulator milestone**.
- If another document proposes broader production hardening, treat it as **post-v0.1.0 backlog** unless it fixes a demonstrated correctness/reliability defect or is explicitly approved.
- Keep the current layered architecture. **Do not rewrite or remove proven concurrency, TCP, resource-ownership, or shutdown mechanisms merely to reduce scope.**

## Why the project is close to release

Milestones 0–5D establish the core: RAII Winsock/socket ownership; multiple TCP sessions; bounded binary framing and typed UTF-8 JSON protocol; registration and request/connection IDs; bounded ingress/outbound queues and WorkerPool; `TelemetryProcessor`/`TelemetryStore`; safe shutdown; unit and localhost integration testing. Milestone 6A adds Windows CI workflow; 6B-1/6B-2 implement structured JSON Lines logging and integration. Hosted CI success must be checked separately; do not infer it from local tests or workflow presence.

The engineering objectives are already demonstrated in the implementation. The remaining objective is **usable demonstration and presentation**, not additional infrastructure.

## Next milestone: 6C — Minimal Runtime Configuration

**Status as of this decision:** design approved with three clarifications; implementation not yet started, according to the latest user-supplied Codex review. Continue the existing Codex conversation; do not start a conflicting 6C.

Approved behavior:
- `telemetry_server --port <N>` accepts 0..65535.
- `telemetry_server --max-connections <N>` accepts 1..`INT_MAX`.
- `telemetry_server --help` is valid only alone and exits without starting the server.
- No arguments preserve existing `TcpServerConfig` defaults (`port = 0`, `max_connections = 64`).
- Exit codes: 0 success, 1 application error, 2 CLI error.
- Small application-private parser; use `std::from_chars` and no new dependencies; preserve existing network API and shutdown design.

Design clarifications:
1. Parser reports **only explicit overrides** (e.g. `std::optional`), not a second copy of configuration defaults; `main()` applies them to `TcpServerConfig`.
2. Parser does **no printing**. It returns success/help/error plus diagnostics; `main()` handles stdout/stderr and exit codes.
3. Add a minimal executable-level `--help` / invalid-argument CTest smoke check if straightforward; do not build a complex test harness just to verify exit code 2.

Implementation must build cleanly with MSVC warnings, pass applicable/full tests, pass diff checks, and produce an independently reviewable commit. Do not add general configuration frameworks, extra flags, or unrelated refactors.

## Proposed remaining milestones (NOT implementation-approved yet)

### 6D — Minimal Device Simulator / Working Demo

Build only the smallest reliable end-to-end CLI demonstration **if an equivalent runnable scenario does not already exist**:
- Connect to the configured localhost TCP server.
- Register one simulated device.
- Submit a few fixed telemetry readings.
- Request and print per-device/metric statistics.
- Display failures clearly, then exit cleanly.

Reuse existing codec/framing/transport facilities as appropriate. No GUI, database, reconnect engine, background generation, extensible CLI framework, device fleet simulation, or extra protocol commands. Keep tests focused on newly introduced risk. Confirm scope and interfaces before implementation; the 6D design is not yet approved.

### 6E — Portfolio Release v0.1.0

- Ensure a clean Windows 10 / Visual Studio 2022 / MSVC / CMake build and full CTest pass.
- Verify GitHub-hosted Windows CI for the release commit, or explicitly document any inability to verify it.
- Update README with project purpose, concise architecture, requirements, reproducible build/run commands, and a small server-plus-client demo transcript.
- Provide one clear architectural flow diagram; document the known limits (e.g. Windows/loopback focus, in-memory aggregates, no persisted raw sample history).
- Check final diff / repository state; create a `v0.1.0` tag only after successful release checks and explicit approval.

The release is done when a reviewer can clone, build, start the server, run the example client, observe accepted readings and statistics, understand the architecture, and see relevant tests passing. Further embellishment is NOT a release requirement.

## Deferred until AFTER v0.1.0

No new GUI/web UI; database/persistence; Docker or Linux port; TLS/authentication; device fleet simulator; new commands; large logging/event expansion; generic configuration system; arbitrary performance optimizations; broad static-analysis/sanitizer expansion; additional stress suites without a concrete defect; speculative abstractions.

**Exception:** A proven bug in correctness, boundedness, shutdown/lifetime safety, test reproducibility, or release functionality may justify a narrowly scoped fix. Explain the evidence and proposed minimum change before implementing.

## Anti-scope-creep rule

For every newly suggested task, ask:
1. Does it fix a demonstrated correctness/safety issue?
2. Is it required to run the basic server+device demo from a clean checkout?
3. Is it required to explain or verify that demo and the current architecture?

If **no** to all three, put it in a post-release backlog; do not add it to the current milestone. Avoid reopening completed architectural decisions without a concrete defect.

## Handoff for future chats

Start from this document and the latest Codex implementation summary. Do not propose a new roadmap or rename 6C. Act as a focused reviewer: distinguish critical correctness issues from optional improvements, protect scope, and push toward a demonstrable `v0.1.0` release. The user continues implementation in the existing Codex conversation and requests review in ChatGPT.
