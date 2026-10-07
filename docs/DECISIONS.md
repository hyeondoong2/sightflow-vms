# Decisions

A log of architecture/design decisions, in the order they were made. Each
entry: Decision / Reason / Alternatives considered / Trade-offs. Append new
entries at the end; do not edit past entries except to correct factual
errors — if a decision changes, record a new entry that supersedes it and say
so.

---

## D1 — Build one phase at a time, defined by `docs/ROADMAP.md`

**Decision:** Only Phase 1 (single RTSP stream → decode → display) is
designed/implemented now. Multi-channel, OpenCV motion detection, SQLite,
REST/WebSocket, and MCP are named as the final direction but are explicitly
out of scope until their own phase.

**Reason:** The user's stated priority is to avoid over-engineering and to
validate the core pipeline (RTSP → FFmpeg → Qt display) before adding
complexity. Designing later phases now would mean designing against
assumptions Phase 1 hasn't validated yet.

**Alternatives considered:** Design the full multi-channel/motion/SQLite/MCP
architecture up front so later phases "slot in" without rework.

**Trade-offs:** Some rework is likely when Phase 2+ begin (e.g. the thread
model will need to change for multiple channels). Accepted deliberately —
rework guided by real Phase 1 results is preferred over speculative
abstractions that may not fit what's actually learned.

---

## D2 — Demux and decode share one worker thread in Phase 1 (no separate packet queue)

**Decision:** `CaptureWorker` runs demux (`av_read_frame`) and decode
(`avcodec_send_packet`/`avcodec_receive_frame`) sequentially on a single
thread. There is no packet queue; only the resulting frame is queued for the
UI thread.

**Reason:** Two threads and a packet queue add complexity (another bounded
queue, another drop policy, another shutdown path) that isn't justified until
there's a measured reason (e.g. decode work causing network reads to stall
long enough to matter).

**Alternatives considered:** Separate demux thread and decode thread, joined
by a bounded packet queue — closer to how a multi-channel design might
eventually shape each channel's pipeline.

**Trade-offs:** If decode work is ever slow enough to delay `av_read_frame`
noticeably, this could cause avoidable network-side hiccups. Not addressed
now — "optimize only after measurement." Revisit if Phase 1 testing shows a
problem.

---

## D3 — Bounded frame queue with drop-oldest policy, no backpressure into decode

**Decision:** The single frame queue between the capture/decode worker and
the UI thread has a small fixed capacity. When full, the oldest frame is
dropped to make room for the newest. The worker never blocks pushing a frame.

**Reason:** This is a live camera view. Showing the most recent frame is more
valuable than showing every frame, and an unbounded queue risks unbounded
memory growth if the UI thread ever stalls. A blocking/backpressured queue
would instead stall decoding and, transitively, network reads.

**Alternatives considered:** Unbounded queue (rejected outright — violates
the no-unbounded-queues rule and risks memory growth). Bounded queue that
blocks the producer when full (backpressure) — rejected because a stalled
UI thread would then stall the network/decode loop too, which is worse for a
live view than dropping a frame.

**Trade-offs:** Frames can be silently dropped under UI stall or load. Judged
acceptable for a live view; would be reconsidered if a future requirement
needs guaranteed frame delivery (e.g. recording), which is out of scope for
Phase 1 (see `docs/REQUIREMENTS.md`).

---

## D4 — RAII wrappers for all FFmpeg resources; no `shared_ptr` for packets/frames

**Decision:** `AVPacket`, `AVFrame`, `AVFormatContext`, and `AVCodecContext`
are each owned through an RAII wrapper (allocate/open on construction,
unref/free/close on destruction). Frames moving through the pipeline use
single ownership (move semantics), not `shared_ptr`.

**Reason:** FFmpeg's C API requires manual alloc/free pairing on every code
path, including error paths — a natural source of leaks and use-after-free
bugs if done by hand at each call site. Single ownership makes it clear at
every point in time who is responsible for a frame's lifetime, which matters
directly for the race/use-after-free risks across the thread boundary.
`shared_ptr`'s atomic refcounting and shared-ownership semantics aren't
needed when only one thread/stage ever holds a frame at a time.

**Alternatives considered:** Manual `av_packet_alloc`/`av_packet_unref` calls
inline at each use site. `shared_ptr<AVFrame>` for convenience when passing
frames around.

**Trade-offs:** RAII wrapper types are a small amount of upfront boilerplate.
Accepted — this is exactly the kind of ownership/lifetime clarity the project
prioritizes over convenience.

---

## D5 — CPU-side YUV→RGB conversion (`sws_scale`) for Phase 1 display, not a shader path

**Decision:** Decoded YUV `AVFrame`s are converted to an RGB buffer on the
CPU via `sws_scale` before being handed to the Qt/QML display item.

**Reason:** Gets a single stream on screen with the least new machinery —
no custom `QSGNode`/shader/`QRhi` work needed to prove the pipeline. Matches
"optimize only after measurement": there's no performance data yet
justifying a GPU conversion path.

**Alternatives considered:** Upload YUV planes directly to the GPU and
convert to RGB in a shader inside a custom `QQuickItem`/`QSGNode`.

**Trade-offs:** `sws_scale` costs more CPU per frame than a shader-based
conversion, and won't scale as-is to many simultaneous channels. Deferred
deliberately — Phase 1 is one stream, so this cost is not yet a proven
problem. Revisit in Phase 2 (multi-channel) if CPU conversion becomes a
bottleneck.

---

## D6 — Minimal UI for Phase 1: no channel-list/config abstractions

**Decision:** Phase 1 UI is just an RTSP URL entry (or fixed URL), a
start/stop control, a connection-state indicator, and the video display. No
channel list, no per-channel configuration model, no settings persistence.

**Reason:** Building UI abstractions for multiple channels before a single
channel's pipeline is proven would be designing for a hypothetical future
requirement, which the project explicitly avoids.

**Alternatives considered:** Build a channel-list data model and settings
persistence now so Phase 2 "just adds more channels" to existing UI.

**Trade-offs:** The Phase 1 UI will very likely be reworked, not extended,
when Phase 2 begins. Accepted as the cost of not over-engineering ahead of
need.

---

## D7 — Frames cross the thread boundary only via bounded `FrameQueue`, never as one Qt signal per frame

**Decision:** The capture/decode worker never emits a queued Qt signal that
carries a decoded frame. Every frame goes through `FrameQueue` (capacity 2,
drop-oldest on push, latest-wins on pop). The UI thread consumes frames by
polling `FrameQueue` from a fixed-interval `QTimer`, not by reacting to a
per-frame signal. The worker's only Qt signal is a low-frequency
connection-state change (`Connecting`/`Connected`/`Error`/`Stopped`) with no
frame payload. `FrameQueue` is specifically the channel for **video frame
data** — it is not the only thing shared across the worker/UI boundary. The
stop flag (`std::atomic<bool>`, D8) and this connection-state signal are
separate, lower-frequency control/state mechanisms that also cross the
boundary, and neither carries frame data.

**Reason:** Qt's queued signal/slot mechanism delivers through the receiving
thread's event queue, which has no capacity limit of its own. Emitting one
frame-carrying signal per decoded frame would make that event queue an
*implicit* unbounded frame queue — exactly the "unbounded queue" the
project's rules forbid — regardless of how carefully `FrameQueue` itself is
bounded. Polling from a timer keeps frame delivery entirely inside the
explicitly bounded, explicitly policy-controlled `FrameQueue`.

**Alternatives considered:** `emit frameReady(videoFrame)` per decoded frame,
connected with `Qt::QueuedConnection`, updating the display in the slot.

**Trade-offs:** Polling means the UI's frame cadence is decoupled from the
decode cadence and driven by the timer interval instead — a frame can sit
briefly before being picked up, and a slow timer interval increases perceived
latency. Accepted: latency is tunable by adjusting the timer interval, and
this is a live view where the drop/latest-wins policy already accepts losing
some frames.

---

## D8 — Shutdown uses FFmpeg's interrupt callback (+ a network timeout as a safety net); no automatic reconnection in Phase 1

**Superseded in part by D19.** The "no automatic reconnection" half of this
decision no longer holds: `CaptureWorker` now retries automatically (D19).
The interrupt-callback/network-timeout shutdown mechanism described below is
unchanged and still accurate — D19 builds on it rather than replacing it.
Left in place rather than rewritten, per this document's own rule (append a
superseding entry, don't edit history).

**Decision:** `RtspSource` installs an `AVIOInterruptCB` on a pre-allocated
`AVFormatContext` (set before `avformat_open_input`, so the connection
handshake itself is interruptible), backed by an `std::atomic<bool>` stop
flag owned by `CaptureWorker`. A network-level timeout option is also set on
the RTSP connection as a safety net for a connection going silently dead with
no stop requested. Automatic reconnection after an `Error` is explicitly not
implemented — an `Error` or a `Stopped` state both end the session, and
restarting requires a new explicit user action.

**Reason:** FFmpeg's C API has no safe way to preempt a blocking call other
than its own interrupt-callback mechanism — there is no safe way to
force-terminate the worker thread mid-syscall. Building shutdown around the
interrupt callback is the only way to guarantee the worker thread can always
eventually exit and be joined. The timeout is a separate, narrower
safeguard: it bounds how long a *silently dead* connection can block even
before anyone asks to stop. Automatic reconnection is a meaningfully separate
feature (retry policy, backoff, when to give up) that hasn't been scoped or
requested for Phase 1; building it now would be exactly the kind of
speculative feature the project avoids.

**Alternatives considered:** Detaching or force-killing the worker thread on
shutdown if it doesn't respond promptly — rejected as unsafe (undefined
behavior around in-flight FFmpeg calls, leaked/corrupted resources, possible
crash). Implementing automatic reconnect-with-backoff now — rejected as
premature; Phase 1's job is to prove the base pipeline, not connection
resilience policy.

**Trade-offs:** A worst-case shutdown is bounded by roughly one
interrupt-callback poll interval (fast) rather than instant, and a dead
connection with no explicit stop can block for up to the configured network
timeout before the worker notices. Both are acceptable, bounded delays,
which is the property that actually matters (no indefinite hang). Users
experiencing a dropped stream must manually restart it in Phase 1 — accepted
as a known, deliberate gap, not an oversight.

---

## D9 — App-owned `VideoFrame` (not `AVFrame`) crosses the worker/UI boundary; `FrameQueue` capacity fixed at 2

**Decision:** A small, move-only, application-defined `VideoFrame` type
(owned pixel buffer + width/height/stride/pixel format) is the only frame
representation that crosses the worker/UI thread boundary, constructed by
`FrameConverter` from an `AVFrame` via `sws_scale`. No `AVFrame` or other
FFmpeg-owned pointer ever crosses threads. `FrameQueue`'s capacity is fixed
at 2.

**Reason:** Keeping FFmpeg types confined to the worker thread means the UI
thread and `FrameQueue` never need to know about FFmpeg's reference-counting
rules (`av_frame_unref`, buffer pools, etc.) at all — the ownership story for
the object that actually crosses threads is entirely in application code and
as simple as "one owned buffer, moved." A capacity of 2 was chosen as an
initial value for bounded memory, bounded live-view latency, absorbing a
very small producer/consumer timing mismatch, and because old frames remain
disposable under the drop-oldest/latest-wins policy — not because it is
claimed to reduce lock contention (that would require measurement that
hasn't been done). It may change if Phase 1 testing gives a concrete reason
to — see `docs/ARCHITECTURE.md` §5.

**Alternatives considered:** Passing `AVFrame*` (or a ref-counted wrapper
around one) directly into `FrameQueue` and doing the YUV→RGB conversion on
the UI thread instead. A larger or configurable `FrameQueue` capacity.

**Trade-offs:** An extra copy/allocation happens at conversion time
(`AVFrame` → `VideoFrame`'s own buffer) that a direct-`AVFrame` approach
might avoid. Accepted: it buys a clean, FFmpeg-free ownership model at the
one point where two threads actually touch the same data, which matters more
in Phase 1 than the cost of one conversion per frame — revisit only if
measurement shows this conversion is a real bottleneck (consistent with D5).

---

## D10 — Phase 1 display-buffer policy: `VideoFrame` owns its buffer until the UI makes its own independent `QImage` copy

**Decision:** `VideoFrame` (D9) owns its pixel buffer until the UI thread has
consumed it. `VideoDisplayItem` constructs its own independent `QImage` —
with `QImage`'s own copied pixel data, not a view over the `VideoFrame`'s
buffer — while that `VideoFrame` is still alive. Only after that independent
copy exists is the previous `VideoFrame` destroyed.

**Reason:** This keeps pixel-buffer ownership completely unambiguous at the
one point where "who owns this memory right now" would otherwise be easy to
get wrong (a `QImage` wrapping foreign memory without copying it requires the
wrapped buffer to outlive the `QImage`, which is exactly the kind of lifetime
coupling this project's ownership rules try to avoid). Simplicity and
correctness are prioritized over avoiding a copy, matching the project's
"measure before optimizing" rule.

**Alternatives considered:** Wrap the `VideoFrame`'s buffer directly in a
`QImage` with a no-op cleanup function (zero-copy), coupling the `QImage`'s
validity to the `VideoFrame`'s lifetime.

**Trade-offs:** One extra pixel-buffer copy per displayed frame
(`VideoFrame` buffer → `QImage`'s own buffer). Accepted for Phase 1 because
ownership stays unambiguous; a zero-copy/shared-backing-storage approach may
be revisited only if profiling shows this copy is a measured, meaningful
bottleneck (consistent with D5).

---

## D11 — Before the worker thread exists, the console orchestrator (not `RtspSource`/`Decoder`) owns `AVPacket`/`AVFrame`

**Decision:** In the current single-threaded console step (no `CaptureWorker`
yet), the code driving the read/decode loop allocates one `AVPacket` and one
`AVFrame` and reuses each for the life of the run (RAII-freed at scope exit).
`RtspSource::readPacket(AVPacket*)` and `Decoder::receiveFrame(AVFrame*)` are
thin pass-throughs with the same signature as `av_read_frame`/
`avcodec_receive_frame` — they fill a caller-owned buffer and do not hold or
own it themselves. `docs/ARCHITECTURE.md` §1's data-flow diagram annotates
these as "owned by `RtspSource`" / "owned by `Decoder`"; that describes the
eventual `CaptureWorker`-based design, not this step's orchestrator, and a
clarifying note was added there pointing back to this entry.

**Reason:** This step's scope is decode/convert correctness in a
single-threaded console program, not the worker-thread architecture. Having
`RtspSource`/`Decoder` internally store the packet/frame would require them
to manage buffer lifetime across calls for no current benefit — the
orchestrator already needs the buffer alive across the `send`/`receive` and
`unref` sequence, and passing it in directly keeps both classes simple
mirrors of the underlying FFmpeg calls they wrap. Restructuring them to own
these buffers now would be designing ahead of the not-yet-built worker
thread, which the project's rules avoid.

**Alternatives considered:** Have `RtspSource` allocate and own the
`AVPacket` internally, returning a reference/pointer from `readPacket()`;
have `Decoder` allocate and own the `AVFrame` internally, returning it from
`receiveFrame()`. Both were rejected for now as unnecessary structural
change for this step — revisit when `CaptureWorker` is actually built, since
its ownership needs may differ once threading and `FrameQueue` exist.

**Trade-offs:** The current code's ownership doesn't literally match the
target diagram in `docs/ARCHITECTURE.md` §1, which could mislead a reader who
doesn't also read this entry. Mitigated by the clarifying note added at the
diagram. Object lifetimes today: the single `AVPacket` and `AVFrame` each
live for the whole run (allocated once before the loop, unref'd every
iteration/every code path, freed via RAII at `main`'s scope exit); the
`AVFormatContext` (owned by `RtspSource`) and `AVCodecContext` (owned by
`Decoder`) each live for the lifetime of their owning object, unaffected by
this decision.

---

## D12 — `FrameConverter` output format is `BGRA32`; `SwsContext` lifecycle uses `sws_getCachedContext`

**Decision:** `FrameConverter` always converts to
`VideoFrame::PixelFormat::BGRA32` — packed, 4 bytes per pixel, in-memory byte
order per pixel `B, G, R, A` (little-endian), no row padding
(`strideBytes() == width() * 4`). Its pixel buffer is a plain
`std::vector<uint8_t>` allocated by `FrameConverter` itself and written to
directly by `sws_scale`, never FFmpeg-owned memory. Conversion never resizes
(`sws_scale` runs at the source frame's own width/height — color-space
conversion only). `FrameConverter` recreates/reuses its `SwsContext` via
FFmpeg's `sws_getCachedContext` (ownership passed in via `release()`, result
captured via `reset()`) rather than manually tracking source width/height/
pixel format itself.

**Reason:** `BGRA32`'s in-memory byte order matches Qt's
`QImage::Format_RGB32`/`Format_ARGB32` on a little-endian machine (Qt's
0xAARRGGBB value is stored in memory as bytes B, G, R, A), so the eventual
Phase 1 display step (D10) can build a `QImage` directly over this layout
with no channel reordering — consistent with D9's "e.g. RGB32" example.
Using `sws_getCachedContext` for the recreate-on-change behavior avoids
`FrameConverter` re-implementing a comparison FFmpeg already provides;
`docs/ARCHITECTURE.md` §2 describes the required behavior (recreate when
width/height/pixel format change) without mandating a specific mechanism.

**Alternatives considered:** Producing `RGBA32` (byte order R,G,B,A) —
rejected, its bytes don't match any native `QImage` format on a
little-endian machine, requiring an extra channel swap later. Manually
tracking source width/height/pixel format as three fields and calling
`sws_getContext`/`sws_freeContext` directly — rejected as duplicating logic
`sws_getCachedContext` already implements correctly.

**Trade-offs:** None identified beyond what D10 already accepts (one copy
per frame into `VideoFrame`'s own buffer). If a future phase needs a
different pixel format (e.g. a GPU path per D5's revisit condition), this
decision would need to be revisited alongside D5/D9/D10.

---

## D13 — `CaptureWorker` built: confirms D11's local-buffer ownership; fixes the RTSP timeout option key from `stimeout` to `timeout`

**Decision:** `CaptureWorker` (owning the stop flag and worker thread, per
D8/§7) now exists, running the RTSP connect/read/decode/convert loop on its
own thread. As D11 anticipated, `RtspSource`/`Decoder`/`FrameConverter` and
the `AVPacket`/`AVFrame` buffers are all local to `CaptureWorker::run()`'s
stack — none of them became members of `RtspSource`/`Decoder` themselves;
only the resulting `VideoFrame` crosses into `FrameQueue`. D11 stands
unchanged. Separately, while wiring `RtspSource`'s `AVIOInterruptCB` (D8),
the RTSP-level socket timeout option key was checked against the actual
FFmpeg this project links (vcpkg `ffmpeg` 8.1): enumerating the linked
`rtsp` demuxer's `AVOption` array directly (`av_find_input_format("rtsp")
->priv_class->option`) shows it exposes `"timeout"` (int64, microseconds)
and has **no** `"stimeout"` option at all. `RtspSource::openInternal()` is
updated from `"stimeout"` to `"timeout"`.

**Reason:** `av_dict_set` on an unrecognized private option is not an error
— `avformat_open_input` simply leaves it unconsumed in the dictionary — so
the previous `"stimeout"` key was silently doing nothing; the §7 network-
timeout safety net was never actually active. `"stimeout"` was the option's
name in older FFmpeg releases and was later renamed to `"timeout"`; since
this project always builds against whatever FFmpeg vcpkg currently resolves
(no pinned historical version), checking the option list of the actual
linked library is the only reliable way to know which key is live, rather
than trusting the option name from memory or from a specific historical
FFmpeg version's documentation.

**Alternatives considered:** Setting both `"stimeout"` and `"timeout"` in the
options dictionary to be version-agnostic — rejected as unnecessary
complexity for a single pinned dependency version (vcpkg's manifest mode
already pins an exact `ffmpeg` build via `builtin-baseline` in
`vcpkg.json`); if that baseline is ever bumped and reintroduces this
mismatch, the same enumeration check should be re-run rather than
defensively setting both keys forever.

**Trade-offs:** None — this is a correctness fix with no downside. Verified
via a temporary throwaway program (deleted after use, not part of the
shipped build) that printed the linked `rtsp` demuxer's full `AVOption`
list; see the conversation this decision was made in for that output.

---

## D14 — Separate `sightflow-server.exe`: async MediaMTX status API, deliberately ahead of `docs/ROADMAP.md`'s phase sequencing

**Decision:** A second, independent executable, `sightflow-server.exe`
(Qt `Core`+`Network` only — no `Qml`/`Quick`, no FFmpeg), is added alongside
the unchanged `sightflow-vms.exe`. It exposes `GET /channels/<name>`, which
asynchronously queries MediaMTX's Control API
(`GET /v3/paths/get/<name>`, `api: true` in `MediaMTX/mediamtx.yml`) and
reports whether that path currently has a connected source (`ready`).
Internals are split into two cooperating classes, mirroring (as class-level
separation only, not thread-level) a network-I/O-vs-state-management split:
`HttpServer`/`ClientConnection` (raw HTTP I/O, knows nothing about MediaMTX)
and `MediaMtxClient`/`ChannelStatusService` (the MediaMTX query and its JSON
shaping, knows nothing about sockets). Everything runs on one thread/one Qt
event loop — no worker-thread pool, no MPSC queue, no separate state-
management thread — because a single channel's query volume doesn't justify
them. The response distinguishes "confirmed not live" (`200`,
`"live": false`) from "could not check" (`503`,
`"error": "mediamtx_unreachable"`) so a caller can never mistake a
MediaMTX outage for a confirmed-empty path; this status is explicitly the
*video source's* state (as MediaMTX sees it), never conflated with whether
`sightflow-vms.exe` itself successfully decoded anything — the two processes
share no state and never communicate.

**Reason:** Requested directly by the user, independent of
`docs/ROADMAP.md`'s phase sequence. `docs/REQUIREMENTS.md` ("Explicitly out
of scope for Phase 1") and `CLAUDE.md` ("Do not pull forward work from later
phases... REST/WebSocket... unless the current roadmap phase explicitly
calls for it") both name REST APIs as out of scope for Phase 1 specifically
— this decision is a deliberate, acknowledged exception made at the user's
explicit direction, not a reinterpretation of that scope boundary. It is
scoped as narrowly as the request: one read-only endpoint, one channel name
passed through verbatim, no config/auth/multi-channel-listing, no change to
`sightflow-vms.exe`.

**Alternatives considered:** Folding this into `sightflow-vms.exe` itself
(rejected — the user asked for a separate process, and it keeps the Qt/QML
display app's process free of a listening network port). Building it on
Qt HttpServer (rejected only because that module isn't installed in this
project's Qt kit — see D15's sibling note below; `QTcpServer` + minimal
hand-rolled HTTP/1.1 GET parsing was used instead, still fully event-loop-
driven). An actual IOCP-style worker-thread-pool + MPSC-queue design
(explicitly rejected by the user as over-engineered for one channel's query
volume).

**Trade-offs:** `docs/ROADMAP.md` and `docs/REQUIREMENTS.md` now describe a
Phase 1 scope that is narrower than what actually exists in `src/server/`.
Not reconciled by rewriting those documents now (out of scope for this
decision) — a reader should treat this entry, not the phase-1-only wording
elsewhere, as authoritative for `src/server/`'s existence and scope until
those documents are explicitly updated.

---

## D15 — `HttpServer`/`ClientConnection` teardown uses `deleteLater()`, never a direct `delete`, from within connection-finished handling

**Decision:** When a `ClientConnection` finishes (its socket disconnects, or
`HttpServer::shutdown()` tears it down), it is removed from `HttpServer`'s
tracking list and always destroyed via `QObject::deleteLater()` — never a
synchronous `delete` (and never implicitly via a `std::unique_ptr` erased
from within a slot). `HttpServer::shutdown()` additionally calls
`ClientConnection::abortConnection()` first, which synchronously aborts the
underlying `QTcpSocket` (releasing the OS socket handle immediately) without
destroying the C++ object, and snapshots+clears its connection list before
touching any connection, so a synchronous `abort()` → `disconnected` →
`finished` re-entry cannot mutate the list mid-iteration.

**Reason:** The first implementation called `delete` (via
`std::vector<std::unique_ptr<ClientConnection>>::erase`) directly from
`HttpServer::onConnectionFinished`, itself invoked synchronously from
`ClientConnection::onDisconnected` — a slot connected to that same
`ClientConnection`'s owned `QTcpSocket`'s `disconnected` signal. That deletes
the `QTcpSocket` (a child of the `ClientConnection` being deleted) while it
is still unwinding its own `disconnected` signal emission — deleting a
`QObject` from within its own signal's call stack is undefined behavior.
This was caught empirically: a manual test firing several concurrent
`GET /channels/test` requests (each connection closing immediately after its
response, per `Connection: close`) reliably crashed the process with a
segmentation fault, where a single sequential request had not (narrow
timing window, not a guaranteed-every-time crash — exactly the kind of bug
that is easy to miss without concurrent testing). `deleteLater()` defers the
actual destruction to the next event-loop iteration, after the signal
emission that triggered it has fully returned, which is the standard safe
pattern for this.

**Alternatives considered:** Keeping immediate `delete` but only ever
triggering it from a freshly-queued event (e.g. `QTimer::singleShot(0, ...)`)
— rejected as reinventing what `deleteLater()` already does. Not aborting the
socket synchronously during `shutdown()` and relying solely on `deleteLater()`
— rejected because the OS socket handle would only be released once the
deferred delete actually runs, which is not guaranteed to happen before
`QCoreApplication::exec()` returns from `aboutToQuit`.

**Trade-offs:** None identified — `deleteLater()` costs one extra event-loop
round-trip before a finished connection's memory is actually freed, which is
irrelevant at this request volume.

---

## D16 — `sightflow-server.exe` decodes one RTSP channel on a dedicated thread (`DecodeWorker`); only a small mutex-guarded `DecodeMetrics` struct crosses into the Qt event-loop thread

**Decision:** `sightflow-server.exe` gains a second, independent piece of
state beyond the MediaMTX status query (D14): `DecodeWorker`, a dedicated
`std::thread` that opens the `test` channel's RTSP URL and runs the same
demux→decode loop shape as `CaptureWorker` (src/CaptureWorker.cpp), reusing
`RtspSource` and `Decoder` directly (both compiled into the `sightflow-server`
target from their existing `src/` sources — not duplicated). No pixel
conversion (`FrameConverter`), no `FrameQueue`, no display: after each
`avcodec_receive_frame` success, only `frame->width`/`frame->height` are read
and the `AVFrame` is unref'd immediately. The *only* thing this thread writes
is `DecodeMetrics` — a plain, Qt-free, mutex-guarded struct (framesDecoded
count, last frame's width/height, a `{Connecting, Running, Error, Stopped}`
state, last error message). The Qt event-loop thread reads it via
`DecodeMetrics::snapshot()` (lock held only for the copy, same discipline as
`FrameQueue`) when answering `GET /channels/<name>/metrics`
(`DecodeMetricsService`, a sibling of `ChannelStatusService` that knows
nothing about MediaMTX). No `AVPacket`, `AVFrame`, or Qt socket object ever
crosses the worker-thread/event-loop-thread boundary — exactly mirroring
D9's "no FFmpeg type crosses the boundary" rule, just with a metrics struct
in place of `VideoFrame`.

The new endpoint's `"source": "decoder"` field is deliberately a different
string from the existing `GET /channels/<name>`'s `"source": "mediamtx"`
(D14) — one reports what MediaMTX sees from the camera side, the other
reports what this process's own decoder actually did; a reader must not
conflate them. Unlike D14's endpoint, this one never returns `503`: there is
no external dependency to be "unreachable" — reading `DecodeMetrics` always
succeeds, and `state` carries whatever actually happened (including
`"error"`).

**Reason:** Requested directly by the user as the next incremental step
after D14/D15, with explicit constraints: reuse `RtspSource`/`Decoder`
rather than duplicating decode logic; never call a blocking FFmpeg function
from the HTTP-handling thread; pass only small app-owned state across the
worker boundary (no `AVPacket`/`AVFrame`/socket objects); no thread pool, no
IOCP, no MPSC queue, no multi-channel, no motion detection. A dedicated
thread is required here for the same reason `CaptureWorker` needs one (D-level
reasoning in `docs/ARCHITECTURE.md` §3): FFmpeg's blocking C calls have no
Qt/event-loop integration, so the only way to keep the HTTP server's event
loop responsive is a real OS thread, with shutdown built on the same
`AVIOInterruptCB` mechanism (§7) `RtspSource` already provides — no new
cancellation mechanism was invented.

**Alternatives considered:** Running the decode loop on the Qt event-loop
thread itself (rejected outright — it would block every HTTP response for as
long as a blocking FFmpeg call takes, violating the user's explicit
constraint). An IOCP-style worker pool feeding an MPSC queue into a single
"logic thread" that owns all server state (the user's own prior-project
shape, explicitly rejected for this step as over-engineered: with exactly
one producer (`DecodeWorker`) and one piece of state, a single mutex-guarded
struct gives the same "exactly one thread touches live frame-producing
state at a time" property an MPSC queue would, without a queue). A Qt
queued signal carrying the metrics struct per decoded frame (rejected for
the same reason as D7: an unbounded implicit queue in Qt's own event
delivery, now for metrics instead of frame data, for no benefit over a plain
mutex since the data is tiny and polled, not streamed).

**Trade-offs:** A request to `GET /channels/<name>/metrics` can observe a
`DecodeMetrics` snapshot that is marginally stale (up to one frame interval
behind) relative to the decode thread's true current state — acceptable, this
is a polled metrics endpoint, not a live stream. `DecodeWorker::stop()` is
called synchronously from the Qt thread during shutdown (`aboutToQuit`) and
blocks it for up to one `AVIOInterruptCB` poll interval (or the RTSP network
timeout in the worst case) — the same bounded, accepted shutdown latency
documented for `CaptureWorker` in `docs/ARCHITECTURE.md` §7/D8, now paid once
more on the server side.

---

## D17 — `DecodeWorker` retries automatically, on its own existing thread; this is a server-only exception to D8's "no auto-reconnect," not a reversal of it

**Decision:** `DecodeWorker` (D16) now retries indefinitely on both a failed
connection attempt and a mid-session stream drop, instead of ending the
thread. The retry loop runs entirely on the *same* worker thread created by
`start()` — no second thread, no thread pool, no IOCP, no work queue of any
kind. On a failure, the worker calls `DecodeMetrics::setRetrying(message)`
(state → `Retrying`) and waits `kRetryIntervalMs` (5000ms, a simple fixed
value, not backoff/jitter) via a `std::condition_variable` that `stop()`
notifies immediately — so a shutdown request ends the wait right away rather
than after up to 5s of idle sleep. Every retry attempt constructs a fresh
`RtspSource`/`Decoder`/`PacketPtr`/`FramePtr`; the previous attempt's are
already destroyed (RAII, scope exit) before the new ones are allocated —
nothing is reused across a retry. Console logging of failures is rate
limited (first failure logged immediately, then only every 10th) so a
long-dead source doesn't fill the log at one line per 5s forever.

`DecodeMetrics` gains a `Retrying` state (distinct from `Connecting`, which
now means "an attempt is in progress right now," for either the first
attempt or any retry) and two new mutating entry points replacing the old
single `setError`: `setRetrying()` (state → `Retrying`, records the failure
reason, leaves `framesDecoded`/`lastFrameWidth`/`lastFrameHeight` untouched)
and `beginRunning()` (state → `Running`, and — this is the semantics
decision the user asked to pin down — resets `framesDecoded` and
`lastFrameWidth`/`lastFrameHeight` to 0 and clears `lastError`).
**`framesDecoded` is therefore scoped to the current/most-recently-ended
connection, never a lifetime total across reconnects.** `GET
/channels/<name>/metrics`'s JSON shape, field names, and the unrelated `GET
/channels/<name>` endpoint (D14) are unchanged; `DecodeMetricsService` now
includes `"error"` whenever `lastError` is non-empty (previously gated on a
now-removed `Error` state) and maps the new `Retrying` state to
`"state":"retrying"`.

**This is scoped to `sightflow-server.exe`'s `DecodeWorker` only.** D8's
decision — the Phase 1 client (`CaptureWorker`, `sightflow-vms.exe`) does
*not* auto-reconnect after `Error`/`Stopped`, and a new session always
requires an explicit user action — is unchanged and still in force for the
client. The two are allowed to differ because they answer different
questions for different consumers: `CaptureWorker` drives a UI a human is
watching live, where silently retrying behind their back was explicitly
rejected in D8; `DecodeWorker` drives a polled HTTP metrics endpoint with no
human watching a retry happen, where the whole point of this change (per the
user's own framing) is "don't make me restart the server just because the
camera wasn't on yet." Neither decision reasons about or constrains the
other. A reader must not cite this entry as grounds to add auto-reconnect to
`CaptureWorker`, nor cite D8 as grounds to revert this one.

**Reason:** Requested directly by the user, with explicit constraints: one
worker thread only (no new thread/pool/IOCP/queue); fixed simple retry
interval; rate-limited logging; fresh FFmpeg resources per retry via RAII;
one pinned meaning for `framesDecoded`; `stop()` must return promptly during
the retry wait, not just during connected I/O; and the client's D8 decision
must not be confused with or silently overridden by this one.

**Alternatives considered:** Exponential backoff with jitter (rejected as
more than "단순한 고정값" asked for; revisit only if a fixed interval proves
operationally too aggressive or too slow). A separate "reconnect manager"
object/thread watching `DecodeWorker` from outside and restarting it
(rejected — this is exactly the kind of extra thread/queue the user asked
not to add; retrying in place on the same thread is strictly simpler). Making
`framesDecoded` a lifetime total across reconnects (rejected per the user's
explicit instruction not to let old decoded data look like evidence the
current video is alive — a monotonically-climbing total would do exactly
that across a reconnect).

**Trade-offs:** A client polling `GET /channels/<name>/metrics` sees
`framesDecoded` drop back to 0 on every reconnect, which looks like a
regression if read as a lifetime counter instead of a per-session one — this
is why the field's semantics are now spelled out in this entry, in
`DecodeMetrics.h`'s own comments, and in `DecodeMetricsService.cpp`, not left
implicit. `lastError` can persist (correctly) through a `Retrying` state and
the following `Connecting` attempt before being cleared by the next
`beginRunning()` — a consumer that treats any non-empty `error` field as "it
is broken right now" rather than "this is what went wrong most recently"
will misread it; `state` is the field that must be read for current
liveness, `error` is context, not an alarm by itself.

---

## D18 — `sightflow-vms.exe` gets a self-contained `ServerStatusModel` (QML_ELEMENT) that polls both server endpoints over Qt's async network API; no coupling to the video path

**Decision:** The client gains `ServerStatusModel` (`src/ServerStatusModel.h`/
`.cpp`), a plain `QObject` with `QML_ELEMENT` (same pattern as
`VideoDisplayItem`) that owns its own `QNetworkAccessManager` and a
`QTimer` (2000ms). On every tick it issues at most one outstanding `GET`
each to `http://127.0.0.1:8080/channels/test` and `.../metrics` — a
non-owning `QNetworkReply*` guard per endpoint (`channelStatusReply_`/
`decodeMetricsReply_`, cleared to `nullptr` as the first action inside that
reply's own `finished` handler, before anything else runs) makes `poll()`
skip issuing a new request for an endpoint whose previous one hasn't
finished yet, so requests never queue up. Each `QNetworkRequest` sets
`setTransferTimeout(2000)` so a hung connection is eventually recovered
without an unconditional wait. `ServerStatusModel` is fully self-contained —
unlike `VideoDisplayItem`, which needs a C++-constructed `FrameQueue*`
handed to it since QML can't build one, `ServerStatusModel`'s dependencies
(server base URL, channel name) are simple hardcoded constants, so it is
declared directly in `src/qml/Main.qml` with no `main.cpp` wiring at all.

Every property is exposed as an explicit (value, reachable) pair —
`mediaMtxLive`/`mediaMtxReachable`/`channelStatusReachable` for `GET
/channels/test`, `decodeState`/`framesDecoded`/`decodeMetricsReachable` for
`GET /channels/test/metrics` — because a request that fails outright
(`sightflow-server.exe` not running, timeout, malformed JSON) must not leave
the previous successful value on screen looking current. Each `finished`
handler mirrors `MediaMtxClient`'s own reachability check (D14): an HTTP
status-code attribute being present means `sightflow-server.exe` answered
at all (its `GET /channels/test` 503 for "MediaMTX unreachable" still counts
as a reachable, successfully-parsed answer from the *server*); its absence,
or a JSON parse failure, means the *Reachable flag goes false and the paired
value resets to a default, not whatever it was before. QML
(`src/qml/Main.qml`) reads these flags and renders "서버 연결 안 됨" /
"MediaMTX 응답 없음" instead of a stale value when the corresponding
*Reachable is false. The status strip's text is explicit that `framesDecoded`
is "서버 측 디코딩" (the server's own `DecodeWorker`'s count, D16/D17) and
that "MediaMTX 송출" (`mediaMtxLive`) is a separate signal from it — the two
must never be read as the same thing, let alone as "frames this window has
displayed."

`ServerStatusModel` shares no object, thread, or queue with the existing
video path (`RtspSource` → `CaptureWorker` → `FrameQueue` →
`VideoDisplayItem`, `docs/ARCHITECTURE.md` §1–5): it does not touch
`FrameQueue`, is never read from or written to by `CaptureWorker`'s thread,
and `sightflow-server.exe` being down affects only its own four properties
going to their unreachable defaults — the RTSP video continues completely
unaffected, since nothing in that path ever depended on this class existing.

**Reason:** Requested directly by the user: a small on-screen indicator of
MediaMTX's publish state and the server's own decode state/frame count,
using Qt's async network API (never blocking the UI thread), without
stacking up duplicate requests, without adding a per-frame network call, a
second decode thread, WebSockets, multi-channel UI, or a general-purpose
networking framework.

**Alternatives considered:** A context property wired up in `main.cpp`
(`engine.rootContext()->setContextProperty(...)`), matching how some Qt/QML
apps expose C++ state — rejected in favor of `QML_ELEMENT` + direct
declaration in QML, since (unlike `VideoDisplayItem`) this class has no
C++-only dependency that QML can't construct itself, so the extra
`main.cpp` wiring would be pure boilerplate. A single combined
"serverReachable" flag covering both endpoints — rejected: the two HTTP
requests are independent and can succeed/fail independently within the same
poll tick (e.g. one endpoint slow, the other fine), so collapsing them would
hide which one actually failed.

**Trade-offs:** Two independent HTTP round trips every 2s instead of one —
accepted, since the two endpoints answer different questions (D14 vs D16)
and combining them would require a new server-side endpoint, out of scope
here. The status strip can show up to one poll interval (2s) of staleness
relative to the server's true current state — acceptable for a status
display, not a control surface.

---

## D19 — `CaptureWorker` retries automatically on its own existing thread; supersedes D8's "no automatic reconnection" for the client

**Decision:** `CaptureWorker` now retries indefinitely on both a failed
connection attempt and a mid-session stream drop, instead of ending the
thread, using the exact retry-loop shape the server's `DecodeWorker`
established (D17): a fixed `kRetryIntervalMs` (5000ms) interruptible wait
via a `std::condition_variable` that `stop()` notifies (with the flag set
under the same mutex the waiter holds, closing the lost-wakeup race D17
already identified and fixed), rate-limited failure logging (first attempt
immediately, then every 10th), and a fresh `RtspSource`/`Decoder`/
`FrameConverter`/`PacketPtr`/`FramePtr` per attempt with the previous
attempt's released by RAII at scope exit before the next one allocates.
This is adapted to `CaptureWorker`'s own boundary objects, not a copy of
`DecodeWorker`'s files: `CaptureWorker` still owns `FrameConverter` and
pushes into `FrameQueue` (which `DecodeWorker` has neither of), and reports
its lifecycle through a new `CaptureState` (a single `std::atomic<State>`,
`{Connecting, Retrying, Running, Stopped}`) rather than `DecodeMetrics`
(`CaptureWorker` has no frame-count/last-size/error-message reporting
requirement — that information already lives server-side via D16/D17, and
duplicating it client-side was not asked for and is not needed to solve the
stale-frame problem below).

**The stale-frame problem and its two-part fix.** Before this change,
nothing told `VideoDisplayItem` that the frame it was holding belonged to a
connection that had already ended — on a drop, the UI thread would simply
keep re-painting the last successfully decoded frame forever, which (once
`CaptureWorker` could reconnect) would have made a stale frame from the
*previous* session indistinguishable from a fresh one from the *new*
session. Fixed in two places, each closing a different window:

1. `FrameQueue` gains `clear()`. `CaptureWorker` calls it the moment a
   session ends (inside the same path that transitions `CaptureState` to
   `Retrying`, before the backoff wait) — so any frame pushed just before
   the drop, and not yet popped by the UI's 33ms-interval timer, cannot
   survive to be popped after the *next* connection succeeds and mistaken
   for one of its frames.
2. `VideoDisplayItem` now also polls `CaptureState` on its existing
   `QTimer` tick (no second timer) via `setCaptureState()`. The moment it
   observes a transition away from `Running`, it clears its own displayed
   `QImage` and repaints immediately — covering the window *during* the
   retry wait, before `FrameQueue::clear()` would even matter again, so the
   last frame disappears right away rather than sitting on screen for the
   retry interval. While not `Running`, it also skips `tryPopLatest()`
   entirely (defensive — the queue should already be empty, but this
   avoids relying on that).

`VideoDisplayItem` exposes this as a new `connectionState` Q_PROPERTY
(`"connecting"`/`"retrying"`/`"running"`/`"stopped"`), and
`src/qml/Main.qml` renders it as centered text ("연결 중..."/"재연결
중...") that disappears once `running`. This is deliberately a *second*,
independent text element from the existing server-status strip
(`ServerStatusModel`, D18): one is this window's own RTSP connection to the
camera; the other is `sightflow-server.exe`'s own decode state reached over
HTTP. Neither reads the other's property, and nothing here depends on
`sightflow-server.exe` running at all.

**Reason:** Requested directly by the user: start the client before the
camera is publishing and have it pick up the stream once it appears,
without restarting the process; keep working through a mid-stream drop the
same way; never show a stale frame as if it were current; close the window
promptly even mid-retry-wait; and do this as a client-side adaptation of
the server's already-reviewed retry shape (D17), not a blind copy, and not
a reversal of D17's own client/server distinction (D17 explicitly reserved
the question of whether the client should ever get this — this entry is
that question being answered, explicitly, now, by the user, for the
client).

**Alternatives considered:** Emitting a Qt signal from `CaptureWorker` for
state changes (would require making the previously Qt-free `CaptureWorker`
a `QObject`) — rejected in favor of polling `CaptureState` from
`VideoDisplayItem`'s *existing* timer tick, identical in spirit to how
`FrameQueue` itself is already polled rather than signaled (D7): one
mechanism, no new Qt dependency added to the capture/decode thread's own
class. Clearing only `FrameQueue` without also having `VideoDisplayItem`
watch `CaptureState` — rejected: the queue being empty doesn't stop
`VideoDisplayItem` from continuing to show whatever `QImage` it already
copied out on an earlier tick, so the stale frame would still linger for up
to the full retry interval. Reusing `DecodeMetrics` verbatim for
`CaptureState` — rejected as carrying fields (`framesDecoded`,
`lastFrameWidth/Height`, `lastError`) this class has no consumer for.

**Trade-offs:** None identified beyond what D17 already accepted for the
same retry shape (fixed-interval, no backoff/jitter; a `stop()` mid-wait is
bounded by, not independent of, the same correctness fix D17 required).
`docs/ARCHITECTURE.md` §1/§3/§6/§7, written when D8 was current, described
a client that never retries; updated alongside this entry to describe the
retry loop, the two-part stale-frame fix, and the shutdown sequence's
interaction with the retry-backoff wait.

---

## D20 — `sightflow-vms.exe` displays exactly two fixed channels ("test", "test2"); each gets its own independent `FrameQueue`/`CaptureState`/`CaptureWorker`, instantiated by literal duplication, not a channel manager

**Decision:** `main.cpp` now constructs two complete, independent
single-channel object sets — `FrameQueue`/`CaptureState`/`CaptureWorker` for
`rtsp://127.0.0.1:8554/test`, and the same three types again for
`rtsp://127.0.0.1:8554/test2` — by literally writing the construction code
twice, not through a loop, array, registry, or any new "channel manager"
type. No class changed: `CaptureWorker`, `CaptureState`, `FrameQueue`, and
`VideoDisplayItem` were already parameterized per-instance (a URL, and
references to one queue/state pair) from the single-channel step, so this
step's entire implementation is two extra local-variable sets in `main.cpp`
plus a second `VideoDisplayItem` in `src/qml/Main.qml` (found by a second,
distinct `objectName` and wired the same way the first already was). The two
channels share no object: channel "test"'s worker thread never touches
"test2"'s queue, state, or display item, and vice versa — a connection
failure, retry, or frame drop on one is invisible to the other by
construction, not by any added guard.

`src/qml/Main.qml` splits the window into two equal-width panes (a `Row` of
two `Item`s, each `width: parent.width / 2`) — proportional, not
fixed-pixel, so a window resize keeps the 50/50 split; each pane's
`VideoDisplayItem` independently preserves its own frame's aspect ratio
within its own pane via `paint()`'s existing `KeepAspectRatio` logic
(unchanged — this already worked per-instance, nothing new was needed for
it). Each pane is labeled with its channel name and shows its *own*
`CaptureState`-derived connection text (D19), not the other pane's. The
existing server-status strip (`ServerStatusModel`, D18) is unchanged in
behavior but its text now explicitly says "[서버 상태 - test 채널 전용]" —
`sightflow-server.exe` in this step still only runs one `DecodeWorker`, for
"test" (D16/D17 unchanged, not extended to "test2" here), so that strip
must not be misread as describing "test2".

**Explicitly out of scope for Phase 1, with a deliberate, narrow exception
— same pattern as D14.** `docs/REQUIREMENTS.md` ("Explicitly out of scope
for Phase 1") names "Multiple simultaneous channels/streams" and
`docs/ROADMAP.md` names Phase 2 as "Multi-channel" specifically. This entry
is that exception, made at the user's explicit direction, scoped as
narrowly as D14's: exactly two hardcoded channel names and URLs, no
channel-list data model, no dynamic add/remove UI, no generic N-channel
infrastructure. It does not pull forward any other part of Phase 2's scope
(still no OpenCV, no per-channel settings, no layout beyond a fixed 50/50
split).

**Reason:** Requested directly by the user as the next incremental step,
with explicit constraints mirroring the project's established "don't
over-engineer" rule applied to multi-channel specifically: each channel
independently owned and unaffected by the other; reuse the existing
single-channel classes unchanged; no `shared_ptr`, no general-purpose
channel manager, no dynamic channel UI, no thread pool; extend the existing
single-channel code with as small a change as possible.

**Alternatives considered:** A `std::vector<ChannelContext>` (or similar)
holding N channels, iterated in a loop, with channel definitions in a data
table — rejected as exactly the "general-purpose channel manager" the user
asked not to build, and as unnecessary for a scope fixed at exactly two
channels; revisit only if/when Phase 2 actually begins and a real channel
count/list requirement is scoped. A `QML_ELEMENT` "channel bundle" to pair
each `VideoDisplayItem` with its own internal `CaptureWorker` — rejected:
`CaptureWorker` must stay off the Qt/QML side entirely (it is not a
`QObject` and should not become one just to live in QML), so ownership of
the worker/queue/state triplet must stay in `main.cpp`, matching the
existing single-channel shape.

**Trade-offs:** `main.cpp` now has two near-identical six-line blocks
instead of one — accepted as the honest cost of "exactly two, hardcoded,
not a loop"; revisit if a third fixed channel is ever requested (at which
point a small helper may become worth it, but that is a decision for that
moment, not now). The server status strip still only reports "test" — a
viewer could still misread "test2"'s true server-side decode state as
unknown/unavailable rather than "not tracked by the server in this step",
even with the clarifying label; a full fix (a second `DecodeWorker`/
`/channels/test2/metrics` on the server) is out of scope here and was not
requested.

---

## D21 — `sightflow-server.exe` and `ServerStatusModel` extended to both fixed channels; fulfills the gap D20 explicitly left open

**Decision:** This closes exactly the gap D20's Trade-offs section named:
`sightflow-server.exe` now runs two independent `DecodeWorker` instances —
one per fixed channel, `test` and `test2` — each with its own
`DecodeMetrics` and `DecodeMetricsService`, constructed by literal
duplication in `main.cpp` (mirroring D20's client-side pattern exactly, not
a new mechanism). `GET /channels/test/metrics` and
`GET /channels/test2/metrics` are both live, same JSON shape and contract
as before (D16/D17) — nothing about the response format changed, only that
a second, fully independent instance of the whole chain now answers for
`test2`. `ChannelStatusService` (D14) needed **no change at all**: it was
already channel-name-generic — it forwards whatever name is in the request
path straight through to `MediaMtxClient::queryPathStatus()` — so
`GET /channels/test2` already worked before this entry, and still does, via
the same single `ChannelStatusService` instance serving both channels.

Routing in `main.cpp` is exact-string matching on the two known metrics
paths (`"/channels/test/metrics"`, `"/channels/test2/metrics"`), not a
generic "strip the suffix and look up a channel registry" dispatcher —
anything else ending in `/metrics` is a plain 404. This is deliberately not
infrastructure for an arbitrary channel count; see Alternatives below.

Shutdown now stops both `DecodeWorker`s from the single `aboutToQuit`
handler, sequentially (`decodeWorkerTest.stop()` then
`decodeWorkerTest2.stop()`) — each call is already bounded (one
interrupt-callback poll, or an immediate `condition_variable` wake if
between retry attempts, D17) and two fixed channels don't justify added
complexity to overlap the two joins.

On the client, `ServerStatusModel` (D18) gains a settable `channelName`
property (default `"test"`, for source compatibility with existing
single-instance behavior) instead of a hardcoded channel name, so
`src/qml/Main.qml` can declare one instance per pane
(`ServerStatusModel { channelName: "test2" }`). Its first poll moved from a
synchronous constructor call to `QTimer::singleShot(0, ...)`: QML assigns
declared properties (including `channelName`) immediately after
construction but *before* control returns to the event loop, so a
same-tick synchronous poll in the constructor would have queried whatever
`channelName_`'s compiled-in default was, never the value QML actually
declared. The previous single, window-wide "[서버 상태 - test 채널 전용]"
strip is replaced by one such status line per pane, each clearly showing
its own channel name and reporting only that channel's MediaMTX/decode
state — not the other pane's, and still clearly distinguished from that
pane's own `VideoDisplayItem.connectionState` text (D19/D20, the client's
own RTSP connection, unrelated to anything in this entry).

**Reason:** Requested directly by the user as the natural next step after
D20 — D20's own Trade-offs section already named this exact gap
("test2"'s server-side decode state unknown/unavailable) and said a fix was
out of scope *then*; it is in scope now. Constraints mirrored D20's:
independent per-channel ownership (a failure/retry/shutdown on one
`DecodeWorker` must not touch the other's thread, metrics, or HTTP
response), no change to the existing response contracts, no general channel
registration system, FFmpeg decoding confined to each worker thread with
`AVPacket`/`AVFrame` never reaching the Qt HTTP thread (already true by
construction -- `DecodeMetrics` is the only thing that crosses that
boundary, unchanged from D16), and no stale "last known good" state shown
when the server or MediaMTX is down (already true by construction -- the
*Reachable flags, unchanged from D18, already force that).

**Alternatives considered:** A `QMap<QString, DecodeWorker*>` (or similar)
keyed by channel name, with a generic metrics-route handler doing a map
lookup — rejected for the same reason D20 rejected a
`std::vector<ChannelContext>`: it is exactly the "범용 채널 등록 시스템"
the user asked not to build, for a scope fixed at exactly two channels.
Giving `ServerStatusModel` a constructor parameter for `channelName`
instead of a settable `Q_PROPERTY` — rejected: `QML_ELEMENT` types are
constructed by QML with their default constructor and then have declared
properties assigned, so a required constructor argument isn't available to
set from QML at all; a property is the only way QML can parameterize an
instance it creates declaratively.

**Trade-offs:** None identified beyond what D16/D17/D18/D20 already
accepted for this same shape applied once; this entry is that shape applied
twice, independently, with no new kind of risk introduced. Shutdown time
for `sightflow-server.exe` is now the sum of two (already small, bounded)
`DecodeWorker::stop()` calls instead of one — not parallelized, accepted as
not worth the added complexity for two fixed channels (see Decision above).

**Bug found and fixed while testing this entry:** `DecodeWorker`'s
rate-limited failure log used a chained `std::cerr << a << b << c << ...`.
With two `DecodeWorker`s actually running concurrently for the first time
(one per channel), their chained output was observed to interleave
character-by-character on the shared stream when both logged at the same
moment (e.g. both failing to connect at startup before either stream was
published) -- each `<<` call is a separate, independently-ordered write
against the other thread's. Fixed by building the full line into one
`std::string` first and writing it with a single `std::cerr <<` call, and
by adding `url_` to the line so a reader can tell which channel it belongs
to even for error messages that don't otherwise mention it. Only the log
output was affected -- `DecodeMetrics` (the data each endpoint actually
reports) was never at risk, since it was already mutex-protected
per-instance (D16).

---

## D22 — "화면 변화 감지" (screen change detection): a lightweight downscaled-thumbnail comparison inside each DecodeWorker, not motion/object detection; deliberate, narrow exception to the OpenCV/motion-detection exclusion

**Decision:** Each `DecodeWorker` (one per fixed channel, D21) now runs a
`ChangeDetector` on its own thread, inline in its existing decode loop,
right after each `avcodec_receive_frame()` success and before that
`AVFrame` is unref'd. `ChangeDetector`:

1. Throttles itself to roughly one comparison every `kCompareInterval`
   (200ms) — most decoded frames are skipped outright.
2. On a comparison, uses its own `SwsContext` (RAII via the exact
   `SwsContextPtr`/`sws_getCachedContext` pattern `FrameConverter` already
   established, D5/D12 — not a new pattern) to downscale the frame to a
   tiny `kSampleWidth`×`kSampleHeight` (32×24) `AV_PIX_FMT_GRAY8` thumbnail.
3. Compares that thumbnail, pixel by pixel, against the previous one: a
   pixel counts as "changed" if its brightness moved by more than
   `kPixelDiffThreshold` (25/255); the comparison's change ratio is
   `changed / total`.
4. Requires the ratio to clear `kChangeRatioThreshold` (0.03 — see the
   measured-not-guessed note below) on at least `kDetectionsRequiredInWindow`
   (2) of the last `kDetectionWindowSize` (3) comparisons before it counts
   as sustained change, then enforces `kEventCooldown` (3000ms) before the
   next event can fire even if the change continues.

Every constant above is named and commented in `src/server/ChangeDetector.h`
itself, not buried in `.cpp` logic — this entry records *why* each was
chosen (restated briefly): 200ms balances responsiveness against not
burning CPU on every decoded frame; a 32×24 thumbnail is cheap to compare
and still holds real spatial structure (not one averaged brightness value);
25/255 and 0.03 both exist to separate genuine scene change from per-pixel
codec noise in an otherwise static picture (0.03's exact value is explained
below — it was measured, not picked a priori); requiring 2-of-the-last-3
rather than 2-strictly-in-a-row rejects a single-comparison fluke while
tolerating one ordinary skipped/duplicate-looking comparison (see the bug
note below); the 3s cooldown turns one sustained change into one event
instead of a flood of near-duplicates roughly every 200ms for as long as it
lasts.

**Bug found and fixed while testing this entry, the same day it was
written — same pattern as D21's log-interleaving fix.** The first
implementation used a *strict* consecutive counter (`kConsecutiveDetections
Required`, reset to 0 on any single comparison below threshold) and a
`kChangeRatioThreshold` of 0.15, chosen by reasoning alone before any
measurement. Verifying against FFmpeg `lavfi` test sources exposed two
real problems, confirmed by temporarily logging every comparison's ratio:

1. **The strict counter never fired at all, against any test source tried**
   (`testsrc`, then `mandelbrot`). Logged ratios alternated almost exactly
   between a real, substantial value and precisely `0` every other
   comparison — i.e. one comparison in every pair compared a frame against
   an effectively identical duplicate, a normal consequence of frame-rate
   padding/low-bitrate frame skipping in both encoders generally and these
   test sources specifically, not a defect in them. That single `0` reset
   the strict counter every time, so two genuinely-changing comparisons
   were never seen as "consecutive" even though real, sustained change was
   clearly present. Fixed by replacing the strict counter with the
   `kDetectionWindowSize`/`kDetectionsRequiredInWindow` sliding-window check
   described above, which tolerates exactly this kind of single ordinary
   miss.
2. **Even after that fix, `testsrc` still produced zero events**, because
   its real (non-duplicate) comparisons measured only ratios around
   0.03-0.05 — `testsrc`'s moving clock digit and scrolling color gradient
   occupy a modest fraction of the frame, and the gradient's hue cycles
   without much brightness change (this check is grayscale-only), so even
   genuine continuous motion produced a smaller ratio than the a-priori
   0.15 guess assumed. `mandelbrot`, by contrast, measured ~0.25-0.37 on its
   real comparisons — confirming the detector logic itself was sound once
   the window fix was in place, and that 0.15 was simply too high a bar for
   `testsrc`-level change specifically. `kChangeRatioThreshold` was lowered
   to 0.03 — just above the exactly-`0.0` a truly static source always
   produces, and at the level real `testsrc` change actually measured — so
   both the static control stream and a genuinely changing one would behave
   as intended in verification (see Test results below). This is exactly
   the kind of value this project's "measure, don't guess" rule (D5) calls
   for; it is not claimed to be right for real camera footage, only for
   what was actually measured here (see Trade-offs).

**What this explicitly is not, and the UI/API wording this decision
requires.** This measures "how much of a small, blurred version of the
picture changed between two samples" — nothing more. It cannot and does not
attempt to distinguish a person from a moving shadow, a lighting change, a
camera auto-exposure adjustment, compression artifacts after a scene cut,
or any other cause of a brightness change covering enough of the frame.
Every surface this feature exposes — `ChangeDetector`'s own doc comments,
`ChangeEventLog`/`ChangeEvent`'s doc comments, `ChangeEventService`'s JSON
(`"source": "change-detector"`, field name `changeRatio` not e.g.
`"confidence"` or `"objectsDetected"`), and `src/qml/Main.qml`'s UI text —
uses the literal label **"화면 변화 감지"** (screen change detection) and
phrasing that describes a measured picture-change ratio, never wording that
implies person/object/motion recognition (e.g. never "사람 감지됨",
"움직임 포착"). This was an explicit user requirement, not a stylistic
choice: a reader of the UI or the API must not come away believing this
is more than it is.

**Per-channel independence and the reconnect-reset requirement.** A fresh
`ChangeDetector` is constructed as a local variable at the same point in
`DecodeWorker::run()` where `metrics_.beginRunning()` already runs (right
after a connection attempt succeeds) — the exact same place `FrameConverter`
is already fresh-per-session on the client side (`CaptureWorker::run()`).
Because its comparison baseline (`previousSample_`) starts empty every time,
the first frame of any new session (including the first frame after a
reconnect) only ever *establishes* that baseline and can never itself be
compared against a stale thumbnail from a different session or a different
channel — this is what directly satisfies "재연결 첫 프레임이 이벤트로
기록되지 않게" without needing an explicit `reset()` call anywhere.
`test` and `test2` each get their own `ChangeDetector` (session-scoped, as
above) and their own `ChangeEventLog` (process-lifetime, D21-style literal
duplication in `main.cpp`) — neither channel's detector state or event
history is ever touched by the other's worker thread.

**The boundary object: `ChangeEventLog`.** Mirrors `FrameQueue`'s exact
bounded-`std::deque`-plus-`std::mutex` shape (`docs/ARCHITECTURE.md` §5):
fixed capacity (`kChangeEventLogCapacity` = 20, in `main.cpp`, one per
channel), oldest dropped first once full, mutex held only for the brief
copy in/out. This is the *only* thing that crosses from `ChangeDetector`'s
work back across the worker/HTTP-thread boundary — a timestamp and a
`double` ratio, never an `AVFrame`, a pixel buffer, or any FFmpeg type.
`GET /channels/<name>/events` (`ChangeEventService`, exact-path-routed in
`main.cpp` exactly like the `/metrics` routes, D21) reads it via
`recentEvents()`, which never blocks — same "small mutex-guarded copy,
never held across FFmpeg or socket I/O" discipline every other
worker-to-HTTP-thread object in this codebase already follows
(`DecodeMetrics`, `FrameQueue`, `CaptureState`). `GET /channels/<name>` and
`GET /channels/<name>/metrics` are completely unchanged — this is a new,
additive endpoint, not a modification to either existing contract.

**Client display.** `ServerStatusModel` (D18/D21) gains a third async query,
`GET /channels/<channelName>/events`, polled on the same timer tick and
following the exact same `*Reachable`-flag-plus-paired-value pattern as the
other two queries (so a server/MediaMTX outage shows "서버 연결 안 됨"
instead of the last successful event count looking current, D18). Each
channel pane in `src/qml/Main.qml` gets its own new status line, below the
existing MediaMTX/decode-state line, reporting only its own channel's
`ServerStatusModel` instance — distinct from that same pane's
`VideoDisplayItem.connectionState` (the client's own RTSP connection, D19)
and from the other pane's everything.

**Reason:** Requested directly by the user as the next incremental step.
`docs/REQUIREMENTS.md` ("Explicitly out of scope for Phase 1") excludes
"OpenCV or any motion/image-analysis processing" outright, and
`docs/ROADMAP.md` names Phase 3 "Motion detection" as a distinct future
phase expected to introduce OpenCV. This entry is a deliberate, narrow
exception to that exclusion, in the same spirit as D14/D20/D21: no OpenCV
was added (the comparison is plain pixel-array arithmetic over an
FFmpeg-produced `sws_scale` thumbnail, no new dependency), no second worker
thread or general-purpose frame-processing queue was added (explicit user
constraint), and the feature is scoped to exactly the two existing fixed
channels via the same literal-duplication shape D20/D21 already
established — not a general detection framework.

**Alternatives considered:** Comparing full-resolution frames directly —
rejected: proportional to resolution× for no benefit, when a tiny thumbnail
already captures "did the picture meaningfully change" just as well and far
more cheaply. Comparing every decoded frame instead of throttling by time —
rejected as unnecessary CPU for no detection-quality benefit, since a real
change persists far longer than one frame interval. A second worker thread
per channel dedicated to frame analysis, fed by a new packet/frame queue
from the existing decode thread — rejected outright per the user's explicit
instruction not to add a worker thread or general-purpose queue before it's
needed; the comparison is cheap enough (a 32×24 byte array, throttled to
5/sec) to run inline on the existing decode thread with no measured
justification for more. Using OpenCV's `absdiff`/`countNonZero` or a
built-in background-subtractor — rejected: pulls in a new dependency this
phase explicitly excludes, for a problem plain-array arithmetic over an
already-available `sws_scale` thumbnail solves adequately at this scope.

**Trade-offs and known false-positive/false-negative risk.** This explicitly
cannot tell a real scene event (a person entering frame) apart from a
large-enough lighting change, a camera auto-exposure/auto-focus adjustment,
or compression artifacts around a keyframe/scene cut — any of these can
cross the same ratio threshold and fire an event; conversely, a real but
small or slow-moving change (something small moving far from the camera, or
a very gradual change) may never cross `kChangeRatioThreshold` and so never
fires one. `kChangeRatioThreshold` = 0.03 was tuned from direct measurement
against synthetic FFmpeg `lavfi` test patterns (`testsrc`, `mandelbrot`),
not real camera footage — see the bug note above for the actual measured
values. Because that threshold is now close to the noise floor
(`kPixelDiffThreshold` already filters per-pixel codec noise, but a very
busy real scene's residual thumbnail-level noise after that filter is
untested), real footage may turn out to need a *higher* threshold than
`testsrc` did to avoid noise-driven false events — this is exactly the kind
of adjustment this project's "optimize/tune only after measurement" rule
(D5) anticipates, not a claim that 0.03 is correct beyond what was measured
here. `changeRatio` in the API is the *triggering comparison's* ratio at
the moment the event fired (after the sliding window already cleared
`kDetectionsRequiredInWindow`) — not an average or a confidence score.
