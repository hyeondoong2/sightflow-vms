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
// increasing per channel, never reused, never meaningful compared across
// channels (docs/DECISIONS.md D23). Continues to increase across a server
// restart too (D24): ChangeEventLog::restoreFromPersisted() seeds the next
// id from whatever was last persisted, so a freshly recorded event can never
// collide with an older one still on disk. `hasSnapshot` is true only if a
// JPEG snapshot of the actual decoded frame at the moment this event fired
// was captured and is still held (see ChangeEventLog::snapshotFor() below)
// -- never set for a fabricated/placeholder image: if the encoder failed
// for any reason, the event is still recorded (the ratio is always known),
// just with no retrievable image.
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
// held only for the brief copy in/out, never across FFmpeg, socket, or disk
// I/O. Event metadata and its snapshot are evicted together, as one deque
// entry, so a snapshot can never outlive (or be looked up for) an event no
// longer in the recent window -- see docs/DECISIONS.md D23.
//
// Written by that channel's DecodeWorker thread (record()), read by the Qt
// event-loop thread (recentEvents(), snapshotFor()) when an HTTP request
// asks for it (ChangeEventService). This remains the *only* thing any HTTP
// response ever reads at runtime -- GET /channels/<name>/events and
// .../snapshot are served entirely from this in-memory copy, never from the
// SQLite database directly (docs/DECISIONS.md D24). Persistence (EventStore,
// D24) is a write-behind mirror layered on top by DecodeWorker, outside this
// class: DecodeWorker calls record() here (as before) and then, separately,
// EventStore::appendAndPrune() with the same ChangeEvent this call returns.
// restoreFromPersisted() is the only way data flows the other way (DB ->
// memory), and only ever once, at startup, before any DecodeWorker thread
// starts recording.
class ChangeEventLog {
public:
    // This log deliberately never stores more history than `capacity`
    // (D22/D23's whole point, unchanged by D24's on-disk mirror, which is
    // pruned to the exact same bound), so it cannot tell "this id never
    // existed" apart from "this id existed but was already evicted" without
    // keeping unbounded history, which the project's no-unbounded-growth
    // rule rules out. Both cases are folded into the single EventNotFound
    // result below -- a correct, documented 404 to a caller either way.
    enum class SnapshotLookup { Found, EventNotFound, NoSnapshot };

    // One stored event: its metadata plus the JPEG bytes captured with it
    // (empty if event.hasSnapshot is false). Public so both this class's own
    // internal storage and EventStore's restoreFromPersisted()/loadRecent()
    // round-trip (D24) can share the exact same shape -- no separate DTO.
    struct PersistedEntry {
        ChangeEvent event;
        std::vector<std::uint8_t> snapshotJpeg; // empty if event.hasSnapshot == false
    };

    explicit ChangeEventLog(std::size_t capacity);

    // Appends one event with the current wall-clock time and the next
    // monotonically-increasing id, dropping the oldest entry (and its
    // snapshot, if any) if already at capacity. `snapshotJpeg` may be empty
    // (encoder failure) -- the event is still recorded either way; only
    // `hasSnapshot`/snapshotFor()'s result differ. Never blocks (no disk
    // I/O happens here -- persisting is the caller's separate, explicit
    // responsibility via EventStore, D24). Returns a copy of the recorded
    // ChangeEvent (not just its id) so the caller has the exact timestamp
    // that was stored, to persist the same value rather than recomputing a
    // slightly different "now".
    ChangeEvent record(double changeRatio, std::vector<std::uint8_t> snapshotJpeg);

    // Replaces this log's contents with `entries` (expected oldest-first,
    // already capacity-bounded by the caller -- EventStore::loadRecent()
    // already applies the same `capacity` this instance was constructed
    // with) and sets the next id to continue after the highest id restored
    // (or 1 if `entries` is empty) -- see docs/DECISIONS.md D24. Re-enforces
    // the capacity bound defensively even though callers are expected to
    // already respect it. Must be called before start() on the DecodeWorker
    // that owns this channel's events -- i.e. single-threaded, before any
    // concurrent record()/recentEvents()/snapshotFor() call is possible on
    // this instance. Still mutex-guarded for defensive consistency.
    void restoreFromPersisted(std::vector<PersistedEntry> entries);

    // Newest-first copy of every held event's metadata only -- never
    // includes image bytes, so this stays cheap even at full capacity.
    // Never blocks.
    std::vector<ChangeEvent> recentEvents() const;

    // Copies out the snapshot JPEG bytes for one event id still held in the
    // current window. Never blocks (mutex held only for the scan + copy, no
    // I/O). `outJpeg` is only modified when this returns Found.
    SnapshotLookup snapshotFor(std::uint64_t id, std::vector<std::uint8_t>& outJpeg) const;

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<PersistedEntry> events_; // oldest at front, newest at back; size() <= capacity_
    std::uint64_t nextId_ = 1;
};
