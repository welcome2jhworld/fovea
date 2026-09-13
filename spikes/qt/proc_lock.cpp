#include <QCoreApplication>
#include <QDir>
#include <QLockFile>
#include <QProcess>
#include <QThread>

#include <cstdio>

namespace {

QString lockPath(const QString &dir)
{
    return QDir(dir).filePath("fovea-core.lock");
}

int service(const QString &dir, int seconds)
{
    QLockFile lock(lockPath(dir));
    lock.setStaleLockTime(std::chrono::seconds(30));
    if (!lock.tryLock(std::chrono::milliseconds(100))) {
        qint64 pid = 0;
        QString host, appName;
        lock.getLockInfo(&pid, &host, &appName);
        std::printf("service: already running pid=%lld app=%s error=%d\n", (long long)pid, qPrintable(appName),
                    int(lock.error()));
        std::fflush(stdout);
        return 3;
    }
    std::printf("service: pid=%lld holding lock for %d s\n", (long long)QCoreApplication::applicationPid(), seconds);
    std::fflush(stdout);
    QThread::sleep(seconds);
    std::printf("service: exiting\n");
    return 0;
}

int launch(const QString &dir, int seconds)
{
    qint64 pid = 0;
    const bool ok = QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                            { "--service", dir, QString::number(seconds) }, dir, &pid);
    std::printf("launch: startDetached=%d pid=%lld\n", ok, (long long)pid);
    return ok ? 0 : 1;
}

int status(const QString &dir)
{
    QLockFile lock(lockPath(dir));
    qint64 pid = 0;
    QString host, appName;
    const bool info = lock.getLockInfo(&pid, &host, &appName);
    const bool free = lock.tryLock(std::chrono::milliseconds(0));
    std::printf("status: lockinfo=%d pid=%lld host=%s app=%s acquired=%d error=%d\n", info, (long long)pid,
                qPrintable(host), qPrintable(appName), free, int(lock.error()));
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("fovea-core");
    const QStringList args = app.arguments();
    if (args.size() < 3)
        return 2;
    const QString mode = args[1];
    const QString dir = args[2];
    const int seconds = args.size() > 3 ? args[3].toInt() : 5;
    if (mode == "--service")
        return service(dir, seconds);
    if (mode == "--launch")
        return launch(dir, seconds);
    if (mode == "--status")
        return status(dir);
    return 2;
}
