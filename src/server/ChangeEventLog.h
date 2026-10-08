#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

// One "화면 변화 감지" (screen change detection) sample a channel's
// ChangeDetector produced. `changeRatio` is the fraction (0.0-1.0) of the
// small comparison thumbnail that differed from the previous one at the
// moment this event fired -- see docs/DECISIONS.md D22 for exactly what
// that does and does not mean (not "a person was detected", just "the
// picture changed by at least this much, for at least this long").
//
// `id` identifies this event within its own channel only -- monotonically
// increasing per ChangeEventLog instance, never reused, never meaningful
// compared across channels (docs/DECISIONS.md D23). `hasSnapshot` is true
// only if a JPEG snapshot of the actual decoded frame at the moment this
// event fired was captured and is still held (see ChangeEventLog::
// snapshotFor() below) -- never set for a fabricated/placeholder image: if
// the encoder failed for any reason, the event is still recorded (the ratio
// is always known), just with no retrievable image.
struct ChangeEvent {
    std::uint64_t id = 0;
    std::chrono::system_clock::time_point timestamp;
    double changeRatio = 0.0;
    bool hasSnapshot = false;
};

// Thread-safe, fixed-capacity holder of one channel's most recent change
// events, each optionally paired with the JPEG snapshot captured at the same
// moment -- mirrors FrameQueue's bounded-deque-with-mutex shape
// (docs/ARCHITECTURE.md §5): oldest dropped first once at capacity, mutex
// held only for the brief copy in/out, never across FFmpeg or socket I/O.
// Event metadata and its snapshot are evicted together, as one deque entry,
// so a snapshot can never outlive (or be looked up for) an event no longer
// in the recent window -- see docs/DECISIONS.md D23.
//
// Written by that channel's DecodeWorker thread (record()), read by the Qt
// event-loop thread (recentEvents(), snapshotFor()) when an HTTP request
// asks for it (ChangeEventService). This is the *only* thing ChangeDetector's
// work (plus, now, the one-shot snapshot encode, D23) crosses the
// worker/HTTP-thread boundary as -- no AVFrame, no live pixel buffer, ever.
class ChangeEventLog {
public:
    // This log deliberately never stores more history than `capacity`
    // (D22/D23's whole point), so it cannot tell "this id never existed"
    // apart from "this id existed but was already evicted" without keeping
    // unbounded history, which the project's no-unbounded-growth rule rules
    // out. Both cases are folded into the single EventNotFound result below
    // -- a correct, documented 404 to a caller either way.
    enum class SnapshotLookup { Found, EventNotFound, NoSnapshot };

    explicit ChangeEventLog(std::size_t capacity);

    // Appends one event with the current wall-clock time and the next
    // monotonically-increasing id, dropping the oldest entry (and its
    // snapshot, if any) if already at capacity. `snapshotJpeg` may be empty
    // (encoder failure) -- the event is still recorded either way; only
    // `hasSnapshot`/snapshotFor()'s result differ. Never blocks. Returns the
    // assigned id.
    std::uint64_t record(double changeRatio, std::vector<std::uint8_t> snapshotJpeg);

    // Newest-first copy of every held event's metadata only -- never
    // includes image bytes, so this stays cheap even at full capacity.
    // Never blocks.
    std::vector<ChangeEvent> recentEvents() const;

    // Copies out the snapshot JPEG bytes for one event id still held in the
    // current window. Never blocks (mutex held only for the scan + copy, no
    // I/O). `outJpeg` is only modified when this returns Found.
    SnapshotLookup snapshotFor(std::uint64_t id, std::vector<std::uint8_t>& outJpeg) const;

private:
    struct Entry {
        ChangeEvent event;
        std::vector<std::uint8_t> snapshotJpeg; // empty if event.hasSnapshot == false
    };

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<Entry> events_; // oldest at front, newest at back; size() <= capacity_
    std::uint64_t nextId_ = 1;
};
