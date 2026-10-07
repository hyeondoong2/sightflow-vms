# Architecture

This document describes the architecture planned for **Phase 1** (single
RTSP stream → decode → display) — the implementation phase, which has not
started yet (the project is currently in Phase 0, documentation/design; see
`docs/ROADMAP.md`). See `docs/REQUIREMENTS.md` for what Phase 1 must do. Do
not extend this design for multi-channel, motion detection, SQLite, or MCP
until their phase begins — see `docs/DECISIONS.md` for why.

**Two-channel exception (D20, D21).** `sightflow-vms.exe` now displays
exactly two hardcoded channels, "test" and "test2" (D20), and
`sightflow-server.exe` independently tracks both the same way (D21). Every
class and mechanism below — client-side (`CaptureWorker`, `CaptureState`,
`FrameQueue`, `VideoDisplayItem`, the retry loop, the thread model, the
shutdown sequence) and server-side (`DecodeWorker`, `DecodeMetrics`,
`DecodeMetricsService`, `ChannelStatusService`, `MediaMtxClient`) — is
unchanged and describes **one channel's** objects and behavior — read it
that way throughout. Both `main.cpp`s simply instantiate their respective
one-channel object sets twice, by literal duplication, not a loop or
manager (D20/D21); the two channels share no object on either side, so
nothing below needs to change to describe them both.
`ChannelStatusService`/`MediaMtxClient` were already channel-name-generic
even before D21 and needed no change at all. This is a narrow,
explicitly-scoped exception to this document's own single-channel framing,
not a redesign for N channels — see D20/D21 for what was deliberately *not*
built (a channel list/manager, dynamic UI).

## 1. Data flow

```
RTSP URL
   │
   ▼
[Capture/Decode worker thread]
   loop while !stopRequested:               (outer retry loop -- D19, supersedes D8)
     CaptureState ← Connecting
     RtspSource.open()                        (pre-allocated AVFormatContext, interrupt callback installed first)
     on failure: CaptureState ← Retrying, FrameQueue.clear(), interruptible
                 wait (fixed interval), then retry this loop -- unless
                 stopRequested, in which case exit
     CaptureState ← Running
     loop while !stopRequested (this connected session):
       av_read_frame()             → AVPacket   (owned by RtspSource)
       if packet belongs to the selected video stream:
         Decoder.sendPacket(packet)             (packet handed to decoder)
         repeat Decoder.receiveFrame() until EAGAIN / EOF / error:
           → AVFrame                (decoded YUV frame, owned by Decoder, scoped to this call)
           FrameConverter.convert() → VideoFrame  (app-owned: pixel buffer + width/height/stride/format)
           FrameQueue.push(frame)   → drop-oldest if at capacity (2)
       else:
         (packet belongs to another stream, e.g. audio — not sent to the decoder)
       AVPacket unref'd/released                (every code path, every iteration)
     on a dropped session (not a stop request): same as "on failure" above --
                 CaptureState ← Retrying, FrameQueue.clear(), wait, retry
   │
   │  (thread boundary — FrameQueue is the only channel used to transfer
   │   video frame data between threads; see below for the separate
   │   control/state mechanisms that also cross this boundary)
   ▼
[Qt UI thread]
   QTimer (fixed interval) ticks
     → reads CaptureState; on a transition away from Running, clears the
       currently displayed image immediately (never shows a stale frame
       from an ended session -- D19)
     → if currently Running: FrameQueue.tryPopLatest() → VideoFrame or
       nothing (non-blocking, never waits)
     → VideoDisplayItem displays it, previous VideoFrame is destroyed
```

**Current implementation state vs. this diagram.** `CaptureWorker` now exists
and owns the dedicated capture/decode worker thread and the stop-request flag,
matching this diagram's left-hand box. The Qt UI thread does not exist yet —
`main.cpp` is a temporary console harness that starts `CaptureWorker`, polls
`FrameQueue::tryPopLatest()` on a fixed interval to print frame info (standing
in for the eventual `QTimer`-driven `VideoDisplayItem`), and then requests
stop; see D13 in `docs/DECISIONS.md`. The `AVPacket`/`AVFrame` buffers are
still allocated once and owned locally within `CaptureWorker::run()`'s stack
(not by `RtspSource`/`Decoder` themselves) — `RtspSource::readPacket()`/
`Decoder::receiveFrame()` remain thin pass-throughs with the same signature as
`av_read_frame`/`avcodec_receive_frame`, filling a caller-owned buffer rather
than returning one they own. See D11 in `docs/DECISIONS.md` for why this
still holds now that `CaptureWorker` exists, and the "owned by
RtspSource"/"owned by Decoder" annotations above describe that same intent —
the object that owns the *loop* those buffers live in, not a claim that
`RtspSource`/`Decoder` store them as member state.

`FrameQueue` (§4–5) is the only channel used to transfer **video frame data**
across the worker/UI thread boundary — it is not the only thing shared
between the two threads, just the only path frame pixel data travels. Two
other, separate mechanisms also cross this boundary, each narrower and
lower-frequency than frame delivery:

- The stop flag (`std::atomic<bool> stopRequested`, §7) — written by
  whichever thread requests a stop, read by the worker loop and by the
  interrupt callback. Carries no frame data, just a boolean.
- A low-frequency connection-state value (`CaptureState`: `Connecting` /
  `Retrying` / `Running` / `Stopped`, D19) — a single `std::atomic<State>`
  written by the worker on each transition, *polled* by `VideoDisplayItem`
  on the same `QTimer` tick that already polls `FrameQueue` (not a Qt
  signal — kept symmetric with how `FrameQueue` itself is polled rather
  than signaled, D7). Carries no frame payload; changes on state
  transitions only, not per frame.

No FFmpeg type (`AVPacket`, `AVFrame`, `AVFormatContext`, `AVCodecContext`)
ever crosses the thread boundary, and the worker never emits one Qt signal
per decoded frame (§3, §5).

## 2. Modules and class responsibilities

- **`RtspSource`**
  Owns the FFmpeg `AVFormatContext` for one stream. Pre-allocates the context
  with `avformat_alloc_context()` and installs the `AVIOInterruptCB` (§6)
  *before* calling `avformat_open_input`, so even the initial connection
  handshake is interruptible. Responsible for opening the RTSP URL, locating
  the video stream index, and reading `AVPacket`s (`av_read_frame`). Closes
  the format context on destruction.

- **`Decoder`**
  Owns the FFmpeg `AVCodecContext` for the video stream. Sends packets
  belonging to the selected video stream (`avcodec_send_packet`) and receives
  decoded frames (`avcodec_receive_frame`). The relationship between packets
  and frames is **not** 1:1 — a single `avcodec_send_packet` call can make
  zero, one, or more frames available (e.g. B-frame reordering), so the loop
  is conceptually:
  ```
  read packet
  → if it belongs to the selected video stream:
      avcodec_send_packet()
      → repeatedly avcodec_receive_frame()
        until EAGAIN / EOF / error
  → release packet
  → continue
  ```
  Packets belonging to any other stream (e.g. audio) are released/unref'd and
  skipped without being sent to the decoder. Closes the codec context on
  destruction.

- **`FrameConverter`**
  Wraps `sws_scale` to convert a decoded `AVFrame` into a `VideoFrame` (§4),
  always at the source frame's own width/height (color-space conversion only,
  no resizing) and always into the single fixed output format
  `VideoFrame::PixelFormat::BGRA32` (D12). It is responsible for allocating
  each `VideoFrame`'s pixel buffer as a plain `std::vector<uint8_t>` — never
  FFmpeg-owned memory — and copying `sws_scale`'s output directly into it, so
  the returned `VideoFrame` is independent of the source `AVFrame` and stays
  valid after that `AVFrame` is unref'd. If the source width, height, or
  pixel format changes between calls (e.g. the stream renegotiates
  resolution mid-stream), its `SwsContext` is recreated: this is delegated to
  FFmpeg's own `sws_getCachedContext`, which reuses the existing context when
  parameters are unchanged and transparently frees/reallocates it otherwise,
  rather than `FrameConverter` hand-tracking those fields itself (D12). This
  is intentionally narrow (one `SwsContext`, one output format) and not a
  general media-format abstraction.

- **`VideoFrame`** (application-owned, crosses the thread boundary — §4)
  A plain, move-only value type: owned pixel buffer plus width, height,
  stride (bytes per row), and pixel format. Contains no FFmpeg types and no
  pointers into FFmpeg-owned memory.

- **`FrameQueue`** (§5)
  A fixed-capacity (2), thread-safe holder of `VideoFrame`s, guarded by a
  mutex. Enforces drop-oldest on push and latest-wins on pop internally, so
  no caller can accidentally make it unbounded or treat it as a FIFO of every
  frame.

- **`CaptureWorker`**
  Owns the worker thread and the atomic stop flag. Orchestrates `RtspSource`
  + `Decoder` + `FrameConverter` in a loop, pushes results into the
  `FrameQueue`, owns the connection/error state machine (§6), and owns the
  `AVIOInterruptCB` wiring used for shutdown (§6).

- **`VideoDisplayItem`** (QML-facing)
  Owns a `QTimer` on the UI thread that, on each tick, calls
  `FrameQueue::tryPopLatest()` and repaints if a new frame was returned. Does
  not own decode logic or FFmpeg types — only ever touches an already
  app-owned `VideoFrame`.

- **UI shell (`main.cpp` / top-level QML)**
  Minimal: an RTSP URL input (or fixed URL for first milestone), start/stop
  control, connection-state display (from the lightweight state signal), and
  the `VideoDisplayItem`. No channel list, no settings persistence.

No class in Phase 1 is designed to be subclassed, extended, or configured for
multi-channel or non-RTSP sources. If Phase 2 needs a different shape, it will
change these classes then — see `docs/DECISIONS.md`.

## 3. Thread model

Two threads only:

1. **Capture/decode worker thread** (owned by `CaptureWorker`) — runs the
   retry → demux → decode → convert → enqueue loop (D19). Never touches
   Qt/QML objects directly.
2. **Qt UI thread** — runs the Qt event loop and a `QTimer` that pulls from
   `FrameQueue` and polls `CaptureState`; renders. Never calls into FFmpeg
   directly.

The worker does **not** emit a queued Qt signal carrying a frame for every
decoded frame. Doing so would hand frame delivery to Qt's event queue, which
has no capacity limit of its own — an idle or backed-up UI event loop would
let queued frame-signals accumulate without bound, silently defeating the
bounded-queue/drop policy this architecture relies on (see D7 in
`docs/DECISIONS.md`). Frame delivery instead happens exclusively through
`FrameQueue`, which the UI thread **polls** on its own schedule (§5). The
only other thing the worker exposes is `CaptureState`, a small state value
(`Connecting`/`Retrying`/`Running`/`Stopped`, D19) the UI thread polls on
the same tick — not a Qt signal, and not per frame, so it carries no
capacity risk either.

## 4. The worker/UI boundary object: `VideoFrame`

No `AVFrame` or other FFmpeg-owned memory crosses the thread boundary.
`VideoFrame` is a small, application-owned, move-only type:

```
VideoFrame
  width         : int
  height        : int
  strideBytes   : int              // bytes per row; == width * 4 for BGRA32 (no padding)
  pixelFormat   : VideoFrame::PixelFormat  // app-level enum; BGRA32 is the one format
                                            // FrameConverter produces (D12)
  pixels        : owned buffer      // sole owner of the memory; freed on destruction
```

It has no copy constructor/assignment (copying a frame buffer is never done
implicitly); only move. This matches D4 (single ownership, no `shared_ptr`)
for the one object type that actually crosses threads.

**Full lifetime, one frame:**

1. `Decoder::receiveFrame()` produces an `AVFrame` (YUV), owned by the RAII
   wrapper inside `Decoder`, scoped to this loop iteration.
2. `FrameConverter::convert(avFrame)` runs `sws_scale`, reading the
   `AVFrame`'s planes and writing into a freshly allocated buffer that it
   immediately hands off, by move, into a new `VideoFrame`.
3. The source `AVFrame` is unref'd/destroyed (RAII, end of loop iteration) —
   its data does not need to survive past step 2, since `sws_scale` already
   copied/converted everything needed into the `VideoFrame`'s own buffer.
4. `FrameQueue::push(std::move(videoFrame))` — ownership moves into the
   queue. If the queue is already at capacity, the oldest queued `VideoFrame`
   is destroyed (its buffer freed) to make room (§5).
5. On a `QTimer` tick, `FrameQueue::tryPopLatest()` moves the newest
   `VideoFrame` out to the UI thread; any older frame(s) it skips over in the
   same call are destroyed at that point, not returned (§5).
6. `VideoDisplayItem` holds the popped `VideoFrame` as "currently displayed."
   **Phase 1 display-buffer policy (deliberately simple, not an
   optimization):** `VideoFrame` owns its pixel buffer until the UI has
   consumed it. While that `VideoFrame` is still alive, the UI creates its
   own independent `QImage` — with `QImage`'s own copied pixel data, not a
   view over the `VideoFrame`'s buffer — and only once that independent copy
   exists is the previous `VideoFrame` destroyed, deterministically freeing
   its buffer. No manual `delete` anywhere in this path — every step is
   RAII/move. This costs one extra copy per displayed frame (`VideoFrame`
   buffer → `QImage`'s own buffer), accepted for Phase 1 because it keeps
   buffer ownership completely unambiguous. A zero-copy or shared-backing-
   storage approach may be revisited later, but only if profiling shows this
   copy is a measured, meaningful bottleneck (consistent with D5's "optimize
   only after measurement").

## 5. Bounded `FrameQueue`: capacity and policy

**Capacity: 2.**

Reasoning: the queue's job is only to absorb the small timing gap between
"worker just decoded a frame" and "UI's next timer tick," not to buffer a
backlog. Capacity 2 was chosen as an initial design choice for these reasons:

- **Bounded memory:** worst-case memory is fixed at two frames (at 1080p
  RGB32, ~8 MB each → ~16 MB worst case), regardless of how long the stream
  runs.
- **Bounded live-view latency:** with drop-oldest/latest-wins, the UI is
  never more than a couple of frames behind "now."
- **Absorbs a very small producer/consumer timing mismatch:** one frame of
  slack lets a push landing just before a pop, or vice versa, proceed without
  either side waiting on the other.
- **Old frames remain disposable:** anything beyond the newest frame is
  stale for a live view and safe to drop.

This is an initial value, not a measured one — no profiling has been done to
justify it over 1 or 3, and the project's rule is not to add capacity
speculatively. It may change if Phase 1 testing/measurement gives a concrete
reason to. (Note: capacity 2 is not claimed to reduce lock contention —
that would require measurement that hasn't been done.)

**Push policy (worker → queue):** if the queue is at capacity, drop the
oldest queued `VideoFrame` and push the new one. The worker never blocks
here.

**Pop policy (queue → UI, on each `QTimer` tick):** `tryPopLatest()` never
blocks and never waits for data. If the queue is empty, it returns nothing
and the UI simply doesn't repaint this tick (redisplays the same content).
If the queue holds more than one frame, it returns only the newest and
discards the rest — the UI should never spend time catching up through a
backlog of stale frames; showing the current state of the world is the only
goal (this is a live view, not a video player — consistent with D3).

**Timer interval:** fixed (e.g. ~33 ms, independent of the source stream's
actual frame rate). The UI polls on its own schedule rather than trying to
stay synchronized to the camera's fps; the drop/latest-wins policy above is
what keeps this correct even when the two rates don't match exactly. No
value has been measured yet — this is a starting point to be adjusted if
Phase 1 testing shows a reason to.

## 6. Error handling

**Automatic retry (D19, supersedes D8's "no automatic reconnection").**
`CaptureWorker` retries indefinitely on both a connection failure and a
mid-stream drop — only an explicit `stop()` (window close) is terminal. Each
retry waits a fixed interval (5s), interruptibly, before trying again; see
§7 for how that wait is interrupted both by a stop request and by nothing
else (it is not woken early by anything stream-related — there is no
"retry now" trigger, only the fixed interval or a stop).

- **Connection failure / timeout on open:** `CaptureState` transitions to
  `Retrying`, `FrameQueue` is cleared (so no leftover frame from before this
  failure can later be mistaken for one from the next successful
  connection), and the worker waits the retry interval before attempting to
  open the RTSP URL again. Not a crash, not terminal.
- **Mid-stream network drop:** detected either by `av_read_frame` returning
  an error, or by the separate RTSP/network-level timeout (§7) expiring
  because the connection has silently gone dead. This timeout is a distinct
  safeguard from the interrupt callback (§7) — it is not "the interrupt
  callback firing due to a timeout." Either way, the worker stops pushing
  frames, exits that session's inner loop, and takes the same
  `Retrying`/clear/wait/retry path as a connection failure above.
- **Decode error on a single frame/packet:** log and skip; do not stop the
  whole stream for one bad frame.
- **Fatal/unexpected errors** (e.g. a frame conversion failure):
  treated as ending the current session — same `Retrying`/clear/wait/retry
  path, not a permanent stop. The process never crashes as a result of a
  single stream's failure.
- Only a user-initiated `Stop` (closing the window) is a terminal state in
  Phase 1; `CaptureState` reports `Stopped` and the worker thread exits for
  good. Nothing else requires a new explicit start action anymore — a
  connection failure or a drop is retried automatically without any UI
  action.

## 7. Shutdown: interrupting blocking FFmpeg calls

FFmpeg's blocking calls (`avformat_open_input` during the RTSP handshake,
`av_read_frame` during streaming) are plain C calls with no built-in
cooperative cancellation — there is no safe way to preempt or kill the thread
mid-call. The only supported way to unblock them is FFmpeg's own interrupt
mechanism, so shutdown is built around that rather than any form of forced
thread termination.

**Two separate safeguards.** Phase 1 relies on two distinct mechanisms that
must not be conflated:

- **Manual stop:**
  `stopRequested` set → `AVIOInterruptCB` returns non-zero on its next poll →
  FFmpeg aborts the in-progress blocking call (I/O aborts).
- **Network timeout:**
  a separate RTSP/network-level timeout (an `AVDictionary` option, not the
  interrupt callback) prevents a connection that has gone silently dead from
  blocking indefinitely, even when no stop has been requested.

These exist for different reasons and fire independently of each other: the
interrupt callback is how an explicit stop request reaches a blocked FFmpeg
call; the network timeout is what protects against a dead connection when
*no one has asked to stop at all*.

**Mechanism — manual stop (interrupt callback):**

- `CaptureWorker` owns an `std::atomic<bool> stopRequested`.
- `RtspSource` pre-allocates its `AVFormatContext` with
  `avformat_alloc_context()` and sets `avFormatContext->interrupt_callback`
  to a small function that reads `stopRequested` — **before** calling
  `avformat_open_input`. Setting it any later would leave the initial
  connection handshake uninterruptible.
- FFmpeg polls this callback periodically during blocking network I/O
  (during both `avformat_open_input` and `av_read_frame`). When it returns
  non-zero, FFmpeg aborts the in-progress operation and returns an error
  (e.g. `AVERROR_EXIT`) instead of continuing to block.

**Mechanism — network timeout (independent safety net):**

- `RtspSource` also sets an RTSP-level socket/read timeout (an
  `AVDictionary` option passed to `avformat_open_input`): the key is
  `"timeout"` (int64 microseconds), confirmed by enumerating the linked
  FFmpeg's `rtsp` demuxer `AVOption`s directly (see D13 in
  `docs/DECISIONS.md`) — the older `"stimeout"` name is not recognized by
  this project's FFmpeg version and would have silently done nothing. This
  is a **safety net for a connection that goes silently dead with no stop
  requested** — without it, a dead socket with no data and no explicit stop
  could block `av_read_frame` indefinitely.
- When this timeout fires, it is *not* the interrupt callback firing and it
  is *not* a shutdown — it is treated as the mid-stream network drop case in
  §6 (`Retrying`, automatic retry after the fixed backoff interval, D19).

**Shutdown sequence:**

1. Stop is requested (window close → `~CaptureWorker()`) → `CaptureWorker`
   sets `stopRequested = true` **while holding `waitMutex_`** (the same
   mutex the retry-backoff wait holds throughout its check-then-wait
   sequence), then calls `waitCv_.notify_all()`. Setting the flag under that
   lock closes the lost-wakeup race: without it, the store could land in the
   narrow window between the retry wait checking the predicate (still
   false) and actually registering as waiting, in which case the
   notification would reach no one yet asleep and this step would have to
   wait out the rest of the retry interval instead of ending immediately
   (D19).
2. Two cases, depending on what the worker thread is doing right now:
   - **Blocked in a connected session's FFmpeg call:** the next poll of the
     `AVIOInterruptCB` sees `stopRequested` and aborts it, returning an
     error rather than continuing to block.
   - **Sleeping in the retry-backoff wait (not connected):** `notify_all()`
     from step 1 wakes it immediately; it re-checks `stopRequested`, sees
     it set, and does not retry.
3. The worker loop's top-of-loop (and inner-loop) checks of `stopRequested`
   cause it to break out of every loop level without taking the
   `Retrying`/wait/retry path a non-stop failure would.
4. `Decoder`, `RtspSource`, `FrameConverter`, the `AVPacket`/`AVFrame`
   wrappers are destroyed (RAII), closing the codec and format contexts --
   whether this was the first connection attempt or the Nth retry, exactly
   the same as any other session end.
5. `CaptureWorker` joins the worker thread. This join is now bounded by
   whichever of "one interrupt-callback poll interval" or "effectively
   immediate wake from the retry-backoff wait" applies — never by a full
   retry interval, a full network timeout, or an indefinite hang.
6. `FrameQueue` is cleared and the UI's currently-displayed frame is reset —
   no stale video frame from the previous session is left on screen. (This
   same clearing also already happens on every `Retrying` transition during
   normal operation, D19 — shutdown is not the only time it runs.)
7. `CaptureState` reports `Stopped` — a terminal state in Phase 1; nothing
   automatically restarts from it. `Retrying` is not terminal (D19
   supersedes that half of D8): it is itself the automatic-retry state, not
   a dead end requiring a new user action.

This guarantees the worker thread can always eventually exit and be joined:
either it is between blocking calls and sees the stop flag directly, it is
inside a blocking call that the interrupt callback (backed, worst case, by
the network timeout) will unblock, or it is asleep in the retry-backoff wait
and `notify_all()` wakes it immediately.

**Stop → Start sequencing.** A new capture session must never begin until
the previous worker thread has completely exited and been joined — Phase 1
never has two worker threads alive for the same view at once. Restated as an
explicit sequence:

```
stop request
→ interrupt FFmpeg (§7 mechanisms above)
→ worker exits its loop
→ CaptureWorker joins the worker thread   (step 5 above — must complete
                                            before anything below happens)
→ FrameQueue is cleared
→ displayed frame / connection-state is cleared/reset
→ only now may a new worker be started
```

**Superseded by D19:** the paragraph above described starting a brand-new
`CaptureWorker` instance after a full stop, which remains true as written
(Phase 1 still only ever constructs one `CaptureWorker`, once, in
`main()`). What changed is that a connection failure or a mid-stream drop no
longer ends the *existing* worker's thread at all — it retries internally
(§6) without ever reaching `Stopped` or needing a new instance. Only an
explicit `stop()` (window close) ends the thread for good; see D19
(supersedes D8's "no automatic reconnection").

## 8. Concurrency hazards considered

- **Race:** `stopRequested` is `std::atomic<bool>`, written by the thread
  requesting stop and read both by the worker loop and by the interrupt
  callback (which FFmpeg invokes on the worker thread itself, not a separate
  thread — so there is no additional cross-thread hazard from the callback
  beyond the atomic read it performs). Connection state exposed to the UI is
  communicated only through Qt's queued signal mechanism, never a shared
  variable read directly from both threads.
- **Deadlock:** no lock is ever held while calling into FFmpeg (blocking
  network I/O, decode) or across a Qt signal emission. `FrameQueue`'s mutex
  is held only for the brief push/pop critical section.
- **Use-after-free:** `FrameQueue` transfers ownership by move on both push
  and pop; nothing retains a pointer/reference into a popped or dropped
  `VideoFrame`. RAII wrappers guarantee FFmpeg objects aren't used after
  being freed on the error/shutdown paths.
- **Unbounded queueing via the wrong mechanism:** addressed structurally by
  routing frames only through the fixed-capacity `FrameQueue`, never through
  a per-frame Qt queued signal (§3, §5, D7).
