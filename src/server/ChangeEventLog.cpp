#include "ChangeEventLog.h"

ChangeEventLog::ChangeEventLog(std::size_t capacity)
    : capacity_(capacity)
{
}

std::uint64_t ChangeEventLog::record(double changeRatio, std::vector<std::uint8_t> snapshotJpeg)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.size() >= capacity_) {
        events_.pop_front(); // drop the oldest event (and its snapshot, if any) to make room
    }

    const std::uint64_t id = nextId_++;
    const bool hasSnapshot = !snapshotJpeg.empty();
    events_.push_back(Entry{
        ChangeEvent{id, std::chrono::system_clock::now(), changeRatio, hasSnapshot},
        std::move(snapshotJpeg)});
    return id;
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
    for (const Entry& entry : events_) {
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
