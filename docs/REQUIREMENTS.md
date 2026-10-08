# Requirements

## What this document is

A description of what SightFlow VMS must do, split into the long-term product
direction and the strict scope of the phase currently being built. See
`docs/ROADMAP.md` for how phases are sequenced, and `docs/ARCHITECTURE.md` for
how Phase 1 is structured internally.

## Product direction (final, not all in scope now)

SightFlow VMS is a video management system that will eventually:

1. Connect to one or more RTSP camera streams.
2. Demux and decode video via FFmpeg.
3. Move decoded frames through a frame pipeline to a Qt/QML UI for live display.
4. Support multiple camera channels displayed simultaneously.
5. Run motion detection on decoded frames.
6. Record event metadata (e.g. motion events, channel, timestamp) and support search, backed by SQLite.
7. Expose system state/events to external tools via MCP (Model Context Protocol).

This is the destination, not a spec for what to build next. Only the current
roadmap phase (`docs/ROADMAP.md`) defines what is actually in scope right now.

## Phase 1 requirements (next phase — implementation not yet started)

The project is currently in Phase 0 (documentation/design stage; see
`docs/ROADMAP.md`). The requirements below define what Phase 1 must do once
its implementation begins.

### In scope

- Connect to **exactly one** RTSP stream, given a URL.
- Demux the stream with FFmpeg (`avformat`) into `AVPacket`s.
- Decode video packets with FFmpeg (`avcodec`) into `AVFrame`s.
- Display decoded frames live in a Qt/QML window.
- Handle the basic connection lifecycle: connect, stream, disconnect/stop.
- Handle the obvious failure cases for a single stream: connection failure, mid-stream network drop, decode error on a bad frame — without crashing.
- Clean shutdown: closing the app (or stopping the stream) releases all FFmpeg and Qt resources deterministically, with no leaked threads or handles.

### Explicitly out of scope for Phase 1

Do not design or implement any of the following as part of Phase 1, even partially or "for later":

- Multiple simultaneous channels/streams. **Exception:** `sightflow-vms.exe`
  displays, and `sightflow-server.exe` independently tracks, exactly two
  hardcoded channels ("test", "test2") at the user's explicit direction
  ahead of this phase boundary — see D20 (client) and D21 (server) in
  `docs/DECISIONS.md`. No channel-list data model, dynamic add/remove UI, or
  general N-channel infrastructure was added on either side; this does not
  widen the exclusion beyond those two fixed channels.
- OpenCV or any motion/image-analysis processing. **Exception:** each
  channel's `DecodeWorker` runs a lightweight "화면 변화 감지" (screen
  change detection) check — a downscaled-thumbnail pixel comparison, no
  OpenCV, explicitly not motion/object detection — at the user's explicit
  direction ahead of this phase boundary; see D22 in `docs/DECISIONS.md`.
  D23 extends this with a per-event id and a small JPEG snapshot of the
  actual decoded frame at the moment each event fires, encoded/decoded via
  FFmpeg (no OpenCV, no Qt image plugin, no new dependency). Does not widen
  this exclusion for anything else; no OpenCV dependency, no object/person
  recognition, was added.
- SQLite or any persistence of events/metadata.
- REST or WebSocket APIs. **Exception:** `sightflow-server.exe`
  (`GET /channels/<name>`, a MediaMTX status query) was added at the user's
  explicit direction ahead of this phase boundary — see D14 in
  `docs/DECISIONS.md`. It does not widen this exclusion for anything else;
  no other REST/WebSocket surface is in scope.
- MCP integration.
- Recording/export of video to disk.
- Authentication, user accounts, multi-user access.
- Configuration UI beyond what's needed to enter one RTSP URL and start/stop.

### Non-functional requirements for Phase 1

- **Correctness over features.** A single stream that decodes and displays correctly, with correct resource cleanup, is the entire bar for Phase 1.
- **No unbounded resource growth.** Memory and thread count must stay bounded regardless of how long the stream runs or how it misbehaves (slow network, renderer stalls, decode errors).
- **Live playback semantics.** This is a live camera view, not a video player — favor low latency and dropping stale frames over buffering/completeness.
- **No crashes on expected failure modes.** Connection refused, timeout, mid-stream disconnect, and malformed packets/frames must be handled without crashing the process.

## Future phases (tracked, not specified in detail here)

Requirements for multi-channel, motion detection, SQLite event search, and MCP
will be written when their roadmap phase begins, so that scope decisions are
made with the benefit of what was actually learned building Phase 1.
