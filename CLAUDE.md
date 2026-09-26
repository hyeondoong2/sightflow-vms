# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

SightFlow VMS is a new C++ video management system (VMS). Final direction:

```
RTSP → FFmpeg demux/decode → frame pipeline → Qt/QML rendering → multi-channel
     → motion detection → SQLite event metadata/search → MCP
```

The project is built one phase at a time. See `docs/ROADMAP.md` for the phase
list and current phase. **Phase 1 scope is a single RTSP stream decoded and
displayed — nothing more.** Do not pull forward work from later phases
(multi-channel, OpenCV, SQLite, REST/WebSocket, MCP) unless the current
roadmap phase explicitly calls for it.

Read before working:
- `docs/REQUIREMENTS.md` — what the system must do, and current-phase scope boundaries.
- `docs/ARCHITECTURE.md` — data flow, modules, thread model, ownership, backpressure.
- `docs/ROADMAP.md` — phase goals, completion criteria, test method.
- `docs/DECISIONS.md` — decisions already made, with reasons and alternatives considered.

## Development rules

- **One phase at a time.** Work only within the current phase's scope as defined in `docs/ROADMAP.md`. Do not design or implement future-phase features early.
- **Do not over-engineer.** No abstractions, interfaces, or configurability built for hypothetical future features. Three similar lines beat a premature abstraction.
- **Design before code.** Explain the architecture and data flow for a change before writing implementation. For unfamiliar concepts, explain as concept → data flow → implementation.
- **Small increments.** Do not generate large amounts of code before the user has understood and approved the design.
- **Ownership and lifetime:**
  - Prefer RAII for every resource (FFmpeg contexts, packets, frames, files, locks). No manual alloc/free pairs scattered through logic.
  - Prefer clear single ownership and move semantics over shared ownership. Avoid `shared_ptr` unless a resource genuinely has multiple concurrent owners with no clear lifetime authority — justify it in `docs/DECISIONS.md` when used.
  - `AVPacket`/`AVFrame` lifetime must be explicit and short: allocate, use, unref/free (via RAII wrapper), on every code path including error paths.
- **Concurrency discipline:**
  - Think through race conditions, deadlocks, and use-after-free for every thread boundary before writing code.
  - Never hold a lock across a blocking call (network I/O, decode).
  - Every blocking wait must have a way to wake on shutdown (no unconditional blocking waits).
- **Queues:** No unbounded queues, anywhere. Every queue has a fixed capacity and an explicit, documented policy for what happens when it's full (see frame-drop policy in `docs/ARCHITECTURE.md`).
- **Performance:** Optimize only after measuring. Do not add complexity (lock-free structures, GPU paths, caching, etc.) speculatively.
- **Docs stay current.** When an architecture decision is made or changed during implementation, record it in `docs/DECISIONS.md` (Decision / Reason / Alternatives / Trade-offs) and update `docs/ARCHITECTURE.md` if the data flow or module boundaries changed.

## Current status

No source code, build system, or dependencies exist yet. This repository currently contains only documentation (`docs/`) and this file. Do not create `src/`, `CMakeLists.txt`, or any implementation files until the user explicitly asks to begin Phase 1 implementation.
