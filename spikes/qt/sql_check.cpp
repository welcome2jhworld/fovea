#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlDriver>
#include <QSqlError>
#include <QSqlQuery>

#include <cstdio>

namespace {

bool exec(QSqlDatabase &db, const QString &sql)
{
    QSqlQuery q(db);
    if (!q.exec(sql)) {
        std::printf("FAIL: %s -> %s\n", qPrintable(sql.left(60)), qPrintable(q.lastError().text()));
        return false;
    }
    return true;
}

QVariant scalar(QSqlDatabase &db, const QString &sql)
{
    QSqlQuery q(db);
    if (!q.exec(sql) || !q.next())
        return {};
    return q.value(0);
}

int run(const QString &dir)
{
    const QString path = QDir(dir).filePath("fovea_spike.sqlite");
    QFile::remove(path);
    QFile::remove(path + "-wal");
    QFile::remove(path + "-shm");

    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "core");
    db.setDatabaseName(path);
    db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=5000");
    if (!db.open()) {
        std::printf("open failed: %s\n", qPrintable(db.lastError().text()));
        return 1;
    }
    std::printf("sqlite_version=%s transactions=%d\n", qPrintable(scalar(db, "select sqlite_version()").toString()),
                db.driver()->hasFeature(QSqlDriver::Transactions));

    std::printf("journal_mode=WAL -> %s\n", qPrintable(scalar(db, "PRAGMA journal_mode=WAL").toString()));
    std::printf("synchronous=NORMAL ok=%d\n", exec(db, "PRAGMA synchronous=NORMAL"));
    std::printf("foreign_keys=ON ok=%d, foreign_keys=%s, busy_timeout=%s\n", exec(db, "PRAGMA foreign_keys=ON"),
                qPrintable(scalar(db, "PRAGMA foreign_keys").toString()),
                qPrintable(scalar(db, "PRAGMA busy_timeout").toString()));

    const QStringList schema = {
        "CREATE TABLE IF NOT EXISTS cameras ("
        " id TEXT PRIMARY KEY,"
        " name TEXT NOT NULL,"
        " rtsp_url TEXT NOT NULL,"
        " created_ns INTEGER NOT NULL) STRICT",
        "CREATE TABLE IF NOT EXISTS recordings ("
        " id INTEGER PRIMARY KEY,"
        " camera_id TEXT NOT NULL REFERENCES cameras(id) ON DELETE CASCADE,"
        " path TEXT NOT NULL,"
        " start_ns INTEGER NOT NULL,"
        " end_ns INTEGER) STRICT",
        "CREATE TABLE IF NOT EXISTS events ("
        " id INTEGER PRIMARY KEY,"
        " camera_id TEXT NOT NULL REFERENCES cameras(id) ON DELETE CASCADE,"
        " recording_id INTEGER REFERENCES recordings(id) ON DELETE SET NULL,"
        " kind TEXT NOT NULL,"
        " ts_ns INTEGER NOT NULL,"
        " payload TEXT) STRICT",
        "CREATE INDEX IF NOT EXISTS idx_recordings_camera_start ON recordings(camera_id, start_ns)",
        "CREATE INDEX IF NOT EXISTS idx_events_camera_ts ON events(camera_id, ts_ns)",
    };

    if (!db.transaction())
        return 1;
    for (const QString &sql : schema)
        if (!exec(db, sql))
            return 1;
    if (!db.commit())
        return 1;
    std::printf("schema created: %d statements\n", int(schema.size()));

    exec(db, "INSERT INTO cameras VALUES ('cam1','Front','rtsp://127.0.0.1:8554/cam1', 1)");
    exec(db, "INSERT INTO recordings (camera_id, path, start_ns) VALUES ('cam1','/rec/a.mp4', 10)");
    exec(db, "INSERT INTO events (camera_id, recording_id, kind, ts_ns) VALUES ('cam1', 1, 'motion', 11)");

    bool orphanRejected = false;
    {
        QSqlQuery bad(db);
        orphanRejected = !bad.exec("INSERT INTO recordings (camera_id, path, start_ns) VALUES ('nope','/x', 1)");
        std::printf("orphan insert rejected=%d (%s)\n", orphanRejected, qPrintable(bad.lastError().databaseText()));
    }

    exec(db, "DELETE FROM cameras WHERE id='cam1'");
    std::printf("after cascade delete: recordings=%d events=%d\n",
                scalar(db, "select count(*) from recordings").toInt(), scalar(db, "select count(*) from events").toInt());
    std::printf("wal file exists=%d\n", QFile::exists(path + "-wal"));

    return orphanRejected ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    std::printf("drivers: %s\n", qPrintable(QSqlDatabase::drivers().join(", ")));
    if (!QSqlDatabase::isDriverAvailable("QSQLITE"))
        return 1;
    const int rc = run(argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("."));
    QSqlDatabase::removeDatabase("core");
    return rc;
}
