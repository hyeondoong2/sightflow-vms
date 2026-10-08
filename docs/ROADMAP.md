# Roadmap

Phases are built one at a time, in order. A phase does not start until the
previous phase meets its completion criteria and the user has approved moving
on. Do not implement or design ahead of the current phase — see the
development rules in `CLAUDE.md`.

**Current phase: Phase 0 (documentation/design stage — in progress).**
Phase 1 (implementation) has not started.

## Phase 0 — Project definition (this stage)

- **Goal:** Establish scope, architecture, and working agreements before any
  code exists.
- **Scope:** `CLAUDE.md` and `docs/REQUIREMENTS.md`, `docs/ARCHITECTURE.md`,
  `docs/ROADMAP.md`, `docs/DECISIONS.md` only. No `src/`, no build system.
- **Completion criteria:** User has reviewed and approved the documentation
  set (this roadmap included).
- **Test method:** Review — no automated test applies to documentation.

## Phase 1 — Single stream: connect, decode, display

- **Goal:** Prove the core pipeline works for exactly one RTSP stream:
  RTSP → FFmpeg demux → `AVPacket` → decode → `AVFrame` → on-screen display.
- **Scope:** As defined in `docs/REQUIREMENTS.md` (Phase 1 requirements) and
  `docs/ARCHITECTURE.md`. One stream, one capture/decode thread, one Qt UI
  thread, bounded frame queue, basic connect/stop/error states. No
  multi-channel, OpenCV, SQLite, REST/WebSocket, or MCP — except
  `sightflow-server.exe`'s MediaMTX-status/decode-metrics/change-event
  endpoints and the client displaying video and status, all now covering
  exactly two fixed channels, "test"/"test2" (D14/D20 client, D21 server),
  and the server's non-OpenCV "화면 변화 감지" thumbnail comparison (D22)
  plus its per-event id and real-decoded-frame JPEG snapshot
  capture/retrieval (D23), all added at the user's explicit direction; see
  `docs/DECISIONS.md`.
- **Step 0 (environment setup):** Exact toolchain versions (MSVC, CMake, Qt,
  FFmpeg) are **TBD** — not fixed by this documentation. They are to be
  selected and verified at the start of Phase 1 implementation, before any
  pipeline code is written, and recorded in `docs/DECISIONS.md` once chosen.
- **Completion criteria:**
  - App connects to a real RTSP URL and displays live decoded video.
  - Stopping the stream and closing the app both shut down cleanly (no
    leaked threads, no crash, no hung process).
  - A connection failure and a mid-stream disconnect are both handled
    without crashing, with a visible state change (not a silent freeze).
  - No unbounded memory/queue growth observed during an extended run.
- **Test method:**
  - Manual test against a real (or test-server) RTSP stream: start, watch
    video render, stop, confirm process exits cleanly.
  - Manual fault injection: point at an invalid/unreachable URL; kill the
    RTSP source mid-stream (e.g. stop the test server) and confirm the app
    reports an error state instead of crashing or hanging.
  - Long-running soak (leave it connected) to visually/manually check for
    growing memory usage or degrading responsiveness.

## Phase 2 — Multi-channel

- **Goal:** Extend the proven single-stream pipeline to multiple concurrent
  channels displayed together.
- **Note:** D20/D21 (`docs/DECISIONS.md`) already display and server-side
  track two hardcoded channels via literal per-channel duplication, on both
  the client and the server, as a narrow Phase 1 exception — not a Phase 2
  implementation. Whether this phase generalizes that shape (and at what
  channel count it stops scaling, on either side) is still an open question
  for when this phase actually begins, not decided by D20/D21.
- **Scope:** To be written as a `docs/REQUIREMENTS.md` update when this phase
  begins. Expected to revisit thread-per-stream vs. shared-thread design,
  and layout/UI for multiple channels — decided with the benefit of what
  Phase 1 actually showed, not speculatively now.
- **Completion criteria:** TBD when phase begins.
- **Test method:** TBD when phase begins.

## Phase 3 — Motion detection

- **Goal:** Run motion detection on decoded frames per channel.
- **Note:** D22 (`docs/DECISIONS.md`) already runs a lightweight, non-OpenCV
  "화면 변화 감지" (screen change detection) check per channel — a
  downscaled-thumbnail pixel comparison, explicitly not motion or object
  detection — as a narrow Phase 1 exception, not a Phase 3 implementation.
  Whether this phase replaces, extends, or runs alongside that check (and
  whether OpenCV changes what D22 already measures) is an open question for
  when this phase actually begins, not decided by D22.
- **Scope:** Expected to introduce OpenCV. Detailed scope written when this
  phase begins.
- **Completion criteria:** TBD when phase begins.
- **Test method:** TBD when phase begins.

## Phase 4 — Event metadata and search

- **Goal:** Persist motion/event metadata and support querying/searching it.
- **Scope:** Expected to introduce SQLite. Detailed scope written when this
  phase begins.
- **Completion criteria:** TBD when phase begins.
- **Test method:** TBD when phase begins.

## Phase 5 — MCP integration

- **Goal:** Expose system state/events to external tools via MCP.
- **Scope:** Detailed scope written when this phase begins.
- **Completion criteria:** TBD when phase begins.
- **Test method:** TBD when phase begins.
