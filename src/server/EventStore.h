#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include <QString>

#include "ChangeEventLog.h"

// Persists each channel's recent "화면 변화 감지" events (metadata + JPEG
// snapshot) to a local SQLite database so they survive a server restart
// (docs/DECISIONS.md D24). Strictly a write-behind mirror of ChangeEventLog:
// ChangeEventLog (in-memory) remains the only thing any HTTP response reads
// at runtime -- GET /channels/<name>/events and .../snapshot are completely
// unchanged by this class's existence (D22/D23 contracts, untouched). This
// class is written to right after each ChangeEventLog::record() and read
// from exactly once per channel, at startup, to repopulate ChangeEventLog
// (via restoreFromPersisted()) before that channel's DecodeWorker thread
// starts.
//
// One EventStore instance owns exactly one QSqlDatabase connection, and
// Qt's QtSql module requires a connection be used only from the thread that
// created it (this is about the underlying driver's own thread-safety, not
// Qt's QObject signal/slot thread affinity -- QSqlDatabase is a plain value
// handle, not a QObject). sightflow-server therefore opens three EventStore
// instances in total, never shared across threads: one transient connection
// on the main thread for the one-time startup load (used before any
// DecodeWorker thread starts, then destroyed), and one long-lived instance
// per DecodeWorker thread (opened at the top of that thread's run(), closed
// when it exits) for that channel's own appendAndPrune() calls. All three
// connect to the same on-disk SQLite file; SQLite itself supports
// concurrent connections from different OS threads/processes to one file --
// WAL journal mode plus a busy_timeout (set on every connection this class
// opens) absorb the rare moment two channels' DecodeWorker threads happen
// to write at nearly the same instant, rather than failing outright.
//
// Never a dependency for live operation (D24's resilience policy): every
// method here reports failure via a bool + QString rather than throwing or
// crashing, and every caller's documented policy is to log the message and
// continue running in memory-only mode for whatever that failure affects --
// RTSP decoding, live video, and the existing HTTP API must keep working
// even if persistence is partially or entirely unavailable (missing SQLite
// driver, unwritable directory, full disk, a corrupt database file, ...).
class EventStore {
public:
    // Opens (creating the file/schema if needed) a connection named
    // `connectionName` against the SQLite file at `dbFilePath`.
    // `connectionName` must be unique within this process's QSqlDatabase
    // connection registry (Qt's connections are a global-by-name registry,
    // not scoped per-object) -- callers use one name per thread/purpose,
    // e.g. "events-test", "events-test2", "events-startup". Returns nullptr
    // and fills errorOut on any failure (driver missing, directory not
    // writable, corrupt file, schema creation failed, ...); callers must
    // treat that as "persistence unavailable this run" (D24), not a fatal
    // error.
    static std::unique_ptr<EventStore> open(const QString& connectionName, const QString& dbFilePath, QString& errorOut);

    // Closes this instance's connection and removes it from QSqlDatabase's
    // registry. Must run on the same thread that constructed this instance.
    ~EventStore();

    EventStore(const EventStore&) = delete;
    EventStore& operator=(const EventStore&) = delete;

    // Loads up to `capacity` most-recent persisted rows for `channel`,
    // returned oldest-first (ready to feed straight into
    // ChangeEventLog::restoreFromPersisted()). An empty result with a true
    // return is not an error -- it just means this channel has no persisted
    // rows yet (first run, or a fresh database). Returns false and fills
    // errorOut only on an actual read failure.
    bool loadRecent(const QString& channel, std::size_t capacity,
        std::vector<ChangeEventLog::PersistedEntry>& outEntries, QString& errorOut);

    // Persists one event (already recorded in the in-memory ChangeEventLog)
    // and prunes this channel's persisted rows back down to `capacity` in
    // the same transaction -- mirrors ChangeEventLog's own
    // oldest-dropped-first policy exactly, so disk and memory are always
    // capped to the same bound. `snapshotJpeg` is stored only when
    // `event.hasSnapshot` is true; otherwise the column is written as SQL
    // NULL, never an empty blob (keeps "no snapshot" unambiguous on
    // restore). Returns false and fills errorOut on failure -- the caller's
    // policy (D24) is to log and continue, never to crash or block decoding
    // or serving on this failing.
    bool appendAndPrune(const QString& channel, const ChangeEvent& event,
        const std::vector<std::uint8_t>& snapshotJpeg, std::size_t capacity, QString& errorOut);

private:
    explicit EventStore(QString connectionName);

    // This connection's unique name in QSqlDatabase's process-wide registry
    // -- looked up via QSqlDatabase::database(connectionName_, false) on
    // every call rather than cached as a QSqlDatabase member, since
    // QSqlDatabase objects are themselves just handles into that registry.
    QString connectionName_;
};
