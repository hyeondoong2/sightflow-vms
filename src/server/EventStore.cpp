#include "EventStore.h"

#include <chrono>

#include <QByteArray>
#include <QMetaType>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace {
constexpr const char* kDriverName = "QSQLITE";

constexpr const char* kCreateTableSql =
    "CREATE TABLE IF NOT EXISTS change_events ("
    "channel TEXT NOT NULL,"
    "event_id INTEGER NOT NULL,"
    "timestamp_ms INTEGER NOT NULL,"
    "change_ratio REAL NOT NULL,"
    "snapshot_jpeg BLOB,"
    "PRIMARY KEY (channel, event_id))";
} // namespace

std::unique_ptr<EventStore> EventStore::open(const QString& connectionName, const QString& dbFilePath, QString& errorOut)
{
    if (QSqlDatabase::contains(connectionName)) {
        // Defends against a stale registration left behind by an earlier
        // failed open() under this same name (e.g. a prior attempt that
        // opened the connection but failed schema creation) -- Qt's
        // connection registry is otherwise just a name->handle map with no
        // automatic cleanup.
        QSqlDatabase::removeDatabase(connectionName);
    }

    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QString::fromLatin1(kDriverName), connectionName);
        db.setDatabaseName(dbFilePath);

        if (!db.open()) {
            errorOut = db.lastError().text();
            db = QSqlDatabase(); // drop this local handle before removeDatabase() below
            QSqlDatabase::removeDatabase(connectionName);
            return nullptr;
        }

        // WAL lets other connections keep reading while one holds the write
        // lock; busy_timeout makes a connection that loses a brief write
        // race wait rather than fail outright -- together these are what
        // make two DecodeWorker threads' independent connections to the
        // same file safe under concurrent writes (D24).
        QSqlQuery walPragma(db);
        walPragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
        QSqlQuery busyPragma(db);
        busyPragma.exec(QStringLiteral("PRAGMA busy_timeout=2000"));

        QSqlQuery createTable(db);
        if (!createTable.exec(QString::fromLatin1(kCreateTableSql))) {
            errorOut = createTable.lastError().text();
            db.close();
            db = QSqlDatabase();
            QSqlDatabase::removeDatabase(connectionName);
            return nullptr;
        }
        // `db`, `walPragma`, `busyPragma`, `createTable` all go out of scope
        // here -- no QSqlQuery/QSqlDatabase handle for this connection
        // survives past this block except the registry entry itself, which
        // EventStore's constructor below re-looks-up by name when needed.
    }

    return std::unique_ptr<EventStore>(new EventStore(connectionName));
}

EventStore::EventStore(QString connectionName)
    : connectionName_(std::move(connectionName))
{
}

EventStore::~EventStore()
{
    {
        QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
        if (db.isOpen()) {
            db.close();
        }
        // `db` must go out of scope before removeDatabase() below -- Qt
        // warns (and refuses to fully remove the connection) if any
        // QSqlDatabase handle referencing it is still alive.
    }
    QSqlDatabase::removeDatabase(connectionName_);
}

bool EventStore::loadRecent(const QString& channel, std::size_t capacity,
    std::vector<ChangeEventLog::PersistedEntry>& outEntries, QString& errorOut)
{
    outEntries.clear();

    QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
    if (!db.isOpen()) {
        errorOut = QStringLiteral("connection not open");
        return false;
    }

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT event_id, timestamp_ms, change_ratio, snapshot_jpeg "
        "FROM change_events WHERE channel = ? ORDER BY event_id DESC LIMIT ?"));
    query.addBindValue(channel);
    query.addBindValue(static_cast<qint64>(capacity));
    if (!query.exec()) {
        errorOut = query.lastError().text();
        return false;
    }

    std::vector<ChangeEventLog::PersistedEntry> newestFirst;
    while (query.next()) {
        ChangeEventLog::PersistedEntry entry;
        entry.event.id = static_cast<std::uint64_t>(query.value(0).toLongLong());
        const qint64 epochMs = query.value(1).toLongLong();
        entry.event.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(epochMs));
        entry.event.changeRatio = query.value(2).toDouble();

        const QByteArray blob = query.value(3).toByteArray();
        entry.event.hasSnapshot = !blob.isEmpty();
        if (entry.event.hasSnapshot) {
            entry.snapshotJpeg.assign(blob.begin(), blob.end());
        }

        newestFirst.push_back(std::move(entry));
    }

    outEntries.assign(newestFirst.rbegin(), newestFirst.rend()); // oldest-first, as restoreFromPersisted expects
    return true;
}

bool EventStore::appendAndPrune(const QString& channel, const ChangeEvent& event,
    const std::vector<std::uint8_t>& snapshotJpeg, std::size_t capacity, QString& errorOut)
{
    QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
    if (!db.isOpen()) {
        errorOut = QStringLiteral("connection not open");
        return false;
    }

    if (!db.transaction()) {
        errorOut = db.lastError().text();
        return false;
    }

    QSqlQuery insert(db);
    insert.prepare(QStringLiteral(
        "INSERT INTO change_events (channel, event_id, timestamp_ms, change_ratio, snapshot_jpeg) "
        "VALUES (?, ?, ?, ?, ?)"));
    insert.addBindValue(channel);
    insert.addBindValue(static_cast<qint64>(event.id));
    insert.addBindValue(static_cast<qint64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(event.timestamp.time_since_epoch()).count()));
    insert.addBindValue(event.changeRatio);
    if (event.hasSnapshot && !snapshotJpeg.empty()) {
        insert.addBindValue(QByteArray(reinterpret_cast<const char*>(snapshotJpeg.data()),
            static_cast<qsizetype>(snapshotJpeg.size())));
    } else {
        // Explicit SQL NULL -- never an empty blob -- so a restored row's
        // "no snapshot" state (hasSnapshot == false) is unambiguous (D24).
        insert.addBindValue(QVariant(QMetaType(QMetaType::QByteArray)));
    }

    if (!insert.exec()) {
        errorOut = insert.lastError().text();
        db.rollback();
        return false;
    }

    // Keep only the newest `capacity` rows for this channel -- the exact
    // same oldest-dropped-first bound ChangeEventLog enforces in memory, so
    // disk usage never grows past what was already measured/accepted there.
    QSqlQuery prune(db);
    prune.prepare(QStringLiteral(
        "DELETE FROM change_events WHERE channel = ? AND event_id NOT IN "
        "(SELECT event_id FROM change_events WHERE channel = ? ORDER BY event_id DESC LIMIT ?)"));
    prune.addBindValue(channel);
    prune.addBindValue(channel);
    prune.addBindValue(static_cast<qint64>(capacity));
    if (!prune.exec()) {
        errorOut = prune.lastError().text();
        db.rollback();
        return false;
    }

    if (!db.commit()) {
        errorOut = db.lastError().text();
        db.rollback();
        return false;
    }
    return true;
}
