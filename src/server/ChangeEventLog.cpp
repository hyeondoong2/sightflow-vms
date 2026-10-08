#include "ChangeEventLog.h"

#include <algorithm>
#include <utility>

ChangeEventLog::ChangeEventLog(std::size_t capacity)
    : capacity_(capacity)
{
}

ChangeEvent ChangeEventLog::record(double changeRatio, std::vector<std::uint8_t> snapshotJpeg)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.size() >= capacity_) {
        events_.pop_front(); // drop the oldest event (and its snapshot, if any) to make room
    }

    const std::uint64_t id = nextId_++;
    const bool hasSnapshot = !snapshotJpeg.empty();
    ChangeEvent event{id, std::chrono::system_clock::now(), changeRatio, hasSnapshot};
    events_.push_back(PersistedEntry{event, std::move(snapshotJpeg)});
    return event;
}

void ChangeEventLog::restoreFromPersisted(std::vector<PersistedEntry> entries)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Defensive: the caller (EventStore::loadRecent) already caps this to
    // `capacity_` and already orders it oldest-first, but re-enforce both
    // here rather than trusting that blindly -- a future caller mistake
    // must not be able to grow this log past its bound or seed nextId_ from
    // the wrong end.
    if (entries.size() > capacity_) {
        entries.erase(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(entries.size() - capacity_));
    }

    std::uint64_t maxId = 0;
    for (const PersistedEntry& entry : entries) {
        maxId = std::max(maxId, entry.event.id);
    }

    events_.assign(std::make_move_iterator(entries.begin()), std::make_move_iterator(entries.end()));
    nextId_ = maxId + 1; // continues after the highest restored id -- never collides with it (D24)
}

std::vector<ChangeEvent> ChangeEventLog::recentEvents() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ChangeEvent> result;
    result.reserve(events_.size());
    for (auto it = events_.rbegin(); it != events_.rend(); ++it) {
        result.push_back(it->event); // metadata only -- never copies snapshotJpeg
    }
    return result;
}

ChangeEventLog::SnapshotLookup ChangeEventLog::snapshotFor(std::uint64_t id, std::vector<std::uint8_t>& outJpeg) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const PersistedEntry& entry : events_) {
        if (entry.event.id != id) {
            continue;
        }
        if (!entry.event.hasSnapshot) {
            return SnapshotLookup::NoSnapshot;
        }
        outJpeg = entry.snapshotJpeg; // one small (<= kMaxEncodedBytes) copy, never I/O
        return SnapshotLookup::Found;
    }
    return SnapshotLookup::EventNotFound;
}
