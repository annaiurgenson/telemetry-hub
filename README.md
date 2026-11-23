# TelemetryHub

TelemetryHub is a working C++20 portfolio project: a Windows localhost TCP server that accepts device telemetry, keeps bounded in-memory statistics, and returns request-correlated responses. The `telemetry_server` and `device_simulator` executables provide a small end-to-end demonstration.

The project demonstrates RAII Winsock ownership, nonblocking TCP I/O, bounded length-prefixed messages, typed UTF-8 JSON, `std::jthread` and cooperative cancellation, bounded queues and backpressure, a fixed worker pool, thread-safe domain processing, and orderly shutdown. The server console shows registration and disconnection counts plus every accepted measurement with its updated statistics. MSVC builds use strict `/W4 /WX` warnings.

## Requirements

- Windows 10 or later, Visual Studio 2022 with the C++ desktop workload and Windows SDK.
- CMake 3.25 or newer and Git.
- Network access during the first configure so CMake FetchContent can download pinned GoogleTest 1.17.0 and nlohmann/json 3.12.0.

## Build and test

From a Visual Studio 2022 Developer PowerShell, configure once, then build and test Release:

```powershell
git clone https://github.com/annaiurgenson/telemetry-hub.git
cd telemetry-hub
cmake --preset windows-msvc
cmake --build build\windows-msvc --config Release --parallel
ctest --test-dir build\windows-msvc -C Release --output-on-failure
```

The `windows-msvc` preset uses Visual Studio 2022 x64 and enables tests. Visual Studio keeps configurations in separate output directories: Release executables are in `build\windows-msvc\Release` and test executables are in `build\windows-msvc\tests\Release`; Debug executables are in `build\windows-msvc\Debug` and `build\windows-msvc\tests\Debug`. Select the matching configuration when building and running CTest. The full suite includes unit tests and localhost integration tests.

### Developer testing

For a broader Debug stress check, this local filter repeats every registered test in the WorkerPool, CommandIngress, TcpServer, and ProtocolSession suites 20 times:

```powershell
ctest --preset windows-msvc-debug --tests-regex '^(WorkerPool|CommandIngress|TcpServer|ProtocolSession)[.]' --repeat until-fail:20 --output-on-failure
```

The local four-suite filter is broader than CI's explicit 19-test regression selection. GitHub Actions runs the complete Debug CTest suite and repeats only those named regression tests 20 times on `windows-2022`. The hosted Windows CI run for commit `56f2ef406e07067471e58948ab8ca1f232eacb38` completed successfully on 2026-11-20. A local pass or workflow file alone does not establish hosted CI success; see the [run attached to the commit](https://github.com/annaiurgenson/telemetry-hub/actions/runs/44722700530).

## Run the demonstration

The simulator registers `demo-sensor-1`, submits temperature readings `21.5`, `22.0`, and `23.5`, prints each server-confirmed reading, then exits. The server keeps aggregate statistics in memory while it runs. Press Enter in the server terminal to stop it cleanly.

To run it in two Windows PowerShell terminals from the repository root, start the server in the first and run the simulator in the second. Run the simulator command again, while the same server remains up, to check the count grows from 3 to 6:

```powershell
# Terminal 1
.\build\windows-msvc\Release\telemetry_server.exe --port 5052

# Terminal 2
.\build\windows-msvc\Release\device_simulator.exe --port 5052
```

Example output from two terminals after a real run (timestamps and thread IDs vary):

Server terminal (excerpt from a real Release run):
```text
{"timestamp":"2026-11-20T10:15:24.498Z","level":"info","thread_id":"13652","event":"device_registered","fields":{"device_id":"demo-sensor-1","active_device_count":1}}
{"timestamp":"2026-11-20T10:15:24.499Z","level":"info","thread_id":"940","event":"telemetry_accepted","fields":{"device_id":"demo-sensor-1","metric":"temperature","value":22,"sample_count":2,"minimum":21.5,"maximum":22,"mean":21.75}}
{"timestamp":"2026-11-20T10:15:24.499Z","level":"info","thread_id":"6268","event":"device_disconnected","fields":{"device_id":"demo-sensor-1","active_device_count":0}}
```

Simulator terminal:
```text
Connected to 127.0.0.1:5052
Registered device demo-sensor-1
Telemetry accepted: temperature=21.50
Telemetry accepted: temperature=22.00
Telemetry accepted: temperature=23.50
```

To compare one reading, match the simulator line `Telemetry accepted: temperature=22.00` with the server record for `demo-sensor-1` and `temperature`, where `value` is `22`. The server shows the aggregate after that reading: count 2, minimum 21.5, maximum 22, and mean 21.75.

Run the simulator again without stopping the server. The next three server records advance the count from 3 to 6; the last one shows `sample_count` 6. Stopping and restarting the server starts a fresh in-memory aggregate at count 1.

To send a finite periodic sequence, provide both `--interval-ms` and `--count`. The first reading is sent immediately after registration; after each ACK, the simulator waits the requested interval before sending the next reading. It cycles through 21.5, 22.0, and 23.5 until it receives the requested number of ACKs. The interval accepts 1..60000 milliseconds and count accepts 1..1000. Omitting both options preserves the three-reading demo above.

For example, with the server still running on port 5052:

```powershell
.\build\windows-msvc\Release\device_simulator.exe --port 5052 --interval-ms 500 --count 6
```

`telemetry_server` also accepts `--max-connections <1..INT_MAX>` and standalone `--help`. With no arguments it retains the existing defaults: port 0 (OS-selected ephemeral loopback port) and 64 maximum active connections. For the paired demo, supply the same nonzero port to both programs. Simulator CLI errors exit 2; connection, timeout, protocol, server, or application failures exit 1; success and help exit 0.

## Architecture

```mermaid
flowchart LR
    D[device_simulator] <-->|4-byte length + UTF-8 JSON over loopback TCP| S[TcpServer / ClientSession]
    S --> H[ProtocolSessionHandler]
    H --> R[DeviceRegistrationRegistry]
    H --> I[CommandIngress<br/>bounded incoming queue]
    I --> W[WorkerPool<br/>fixed workers, bounded task queue]
    W --> P[TelemetryProcessor]
    P <--> T[TelemetryStore<br/>bounded in-memory aggregates]
    W -->|correlated response sink| S
```

Each `TcpServer` session owns its socket, reader, and writer; the writer alone sends on that connection. Protocol callbacks validate frames and requests, require registration for telemetry commands, and route work into `CommandIngress`. Worker responses retain the request ID and server-generated `ConnectionId`; a device ID is never used for response routing. Responses may complete out of order, so clients correlate by request ID. The simulator sends one request at a time and validates each response before continuing.

Queues and connections are bounded. The incoming command queue and WorkerPool queue each hold at most 1,024 tasks; the pool has four workers. Each session has a bounded outbound queue and the server accepts at most 64 active connections by default. Incoming-queue saturation returns `SERVER_BUSY`; a connection whose outbound queue cannot accept a response may be closed. Workers never write sockets directly.

Shutdown closes and joins command ingress, drains and joins accepted WorkerPool tasks while the server remains available, then stops sessions and joins their I/O threads before releasing sockets and Winsock. The application keeps the store, processor, response sink, and logger alive through the work that borrows them.

## Protocol

Each TCP frame has a four-byte unsigned network-order payload length and a UTF-8 JSON payload of 1–65,536 bytes. Protocol version is `1`. Request IDs are canonical decimal strings on the wire and `std::uint64_t` in C++; responses preserve the ID. Successful responses contain `result.type`; failures contain a structured `error`.

Example request and response:

```json
{"version":1,"request_id":"43","command":"SUBMIT_TELEMETRY","metric":"temperature","value":23.75}
{"version":1,"request_id":"43","result":{"type":"TELEMETRY_ACCEPTED"}}
```

Devices must register before telemetry submission or statistics queries, and a device may have only one active connection. `ConnectionId` identifies the server-side connection independently from both the device ID and request ID. The codec validates message shape, field types, UTF-8, identifiers, finite values, and canonical IDs; the payload limit is enforced by the framing and codec layers.

`GET_STATISTICS` remains available to protocol clients that need an explicit statistics response. The normal simulator demo omits that final query because the server logs the updated aggregate after each accepted measurement.

## Logging and project status

The server writes synchronous structured JSON Lines lifecycle records, device registration/disconnection counts, accepted measurements with the aggregate snapshot for that update, and selected fatal network/dispatcher failures to `std::clog`. Logging is intentionally limited; output is synchronous and may delay a thread that emits a record.

Milestones 0–5D, 6A–6E, and 7A are complete. Milestone 6A provides Windows CI, 6B structured logging, 6C server runtime configuration, 6D the device simulator, 6E the v0.1.0 release verification, and 7A optional periodic simulator sending. See [docs/PROJECT_PLAN.md](docs/PROJECT_PLAN.md) for development history and detailed engineering decisions, and [docs/MVP_RELEASE_SCOPE.md](docs/MVP_RELEASE_SCOPE.md) for the v0.1.0 scope decision.

## v0.1.0 limitations

- Windows and IPv4 loopback only; no TLS, authentication, or remote-device access.
- Statistics are bounded, in-memory aggregates. Raw readings and state are not persisted, and data is lost when the server exits.
- The simulator uses one fixed device ID, metric, and three readings. It does not generate data in the background, reconnect, or simulate a fleet.
- Client connect and request/response operations have fixed five-second deadlines; the server itself continues running until Enter is pressed in its terminal.
