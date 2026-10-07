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
