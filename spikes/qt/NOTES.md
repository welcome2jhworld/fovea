# Spike 2: Qt 6.9 feasibility for the Fovea process split

Environment: macOS 26.3 arm64, Apple clang (Xcode 26.6, SDK 26.5), CMake 4.0.1, Ninja 1.12.1,
Homebrew Qt 6.9.0 at /opt/homebrew/opt/qt (frameworks), C++20.
Build was done out of tree in the scratchpad (2.3 MB total) and deleted afterwards.

```
cmake -S spikes/qt -B <scratch>/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt
cmake --build <scratch>/build
```

Sources kept here: `CMakeLists.txt`, `shm_ring.cpp`, `http_api.cpp`, `sql_check.cpp`, `proc_lock.cpp`, `tst_smoke.cpp`.

## 1. Shared memory frame ring (QSharedMemory, PosixRealtime)

Verified with `shm_ring --probe`, then `shm_ring --writer` and `shm_ring --reader` as two processes.

Probe output (verbatim):

```
isKeyTypeSupported(PosixRealtime) = 1
isKeyTypeSupported(SystemV) = 1
isKeyTypeSupported(Windows) = 0
DefaultTypeForOs = PosixRealtime, legacyDefaultTypeForOs = SystemV
atomic<uint32_t>::is_always_lock_free = 1, atomic_ref<uint32_t>::is_always_lock_free = 1, required_alignment = 4
atomic_ref<uint64_t>::is_always_lock_free = 1
SystemV create(12 MB) = 0 error=2 "QSharedMemory::handle: system-imposed size restrictions"
SystemV create(3 MB) = 1 error=0 ""
PosixRealtime create(12 MB) = 1 size=12582912 error=0 "" nativeKey=/fovea-spike-ring
atomic_ref in mapped memory: is_lock_free=1 fetch_add ok=1
after creator detach: posix object exists = 1
```

Writer (100 frames, 1280x720 BGRA, 3 slots, 25 fps) and reader (1 ms poll, copies newest slot, seqlock retry):

```
reader: attached after 1 attempts, size=12582912, writerPid=95221, slots=3 1280x720
reader: frames_read=93 torn_reads=0 payload_mismatch=0 skipped=0 exit=writerDone flag
reader: latency us min=152.0 avg=1408.7 max=2662.0
post: writer alive=0, mapped data still readable magic=0x464f5645 latest=1
post: posix object exists after writer exit = 1
post: fresh QSharedMemory::attach after writer exit = 1 error=0 ""
post: fresh QSharedMemory::create after writer exit = 0 error=4 "QSharedMemory::attach (shm_open): already exists"
post: posix object exists after reader detach = 1
post: posix object exists after shm_unlink = 0
```

Findings:

- `QNativeIpcKey::Type::PosixRealtime` is supported and a 12 MB segment is created fine
  (`/fovea-spike-ring`, `size()` reports 12582912). SystemV fails at 12 MB with `InvalidSize`
  because `kern.sysv.shmmax=4194304` (and `shmall=1024` pages = 4 MB total); 3 MB SystemV works.
  `Type::Windows` is unsupported on macOS as expected. Note the legacy default on macOS is still
  SystemV, so always pass the type explicitly.
- Key: use an explicit native key `QNativeIpcKey(QStringLiteral("/fovea-..."), QNativeIpcKey::Type::PosixRealtime)`.
  macOS limits POSIX shm names to `PSHMNAMLEN = 31` chars.
- Reader attaches while the writer holds the segment (1 attempt). The reader missed frames 1..7
  only because it started 300 ms late; after that no gaps (`skipped=0`).
- Seqlock (`std::atomic_ref<uint32_t>` over a plain `uint32_t` in the mapped struct; odd = writing)
  worked: 0 torn reads, 0 payload mismatches. `std::atomic_ref<uint32_t/uint64_t>` is lock free
  and `is_lock_free()` is true on the mapped page; no placement new needed, the writer zero-fills
  and stamps the header. `std::atomic<T>` placed in the segment would also be lock free, but
  `atomic_ref` avoids constructing objects in memory the other process merely maps.
- Latency (writer stamp to reader after a 3.7 MB memcpy) avg 1.4 ms, min 0.15 ms, max 2.7 ms,
  dominated by the 1 ms poll sleep plus the copy. A console that renders in place instead of
  copying, or that waits on a QSystemSemaphore/QLocalSocket notification, will be lower.
- Writer exit: the mapping in the reader stays valid (memory still readable after the writer is
  gone). Qt's POSIX backend does NOT `shm_unlink` on detach on macOS (only QNX does), so the
  object outlives both processes, a later `attach()` still succeeds, and a later `create()` fails
  with `AlreadyExists`. QSharedMemory has no unlink API, so Fovea must own the lifecycle:
  `shm_unlink(name)` before `create()` (stale segment after a crash) and again on clean shutdown.
  On Windows (`Type::Windows`, `CreateFileMappingW`) the kernel frees the mapping when the last
  handle closes, so no stale-object problem there.
- Reader detection of a dead writer: header carries `writerPid` and a per-frame `heartbeat`;
  the spike checks `kill(pid, 0) == -1 && errno == ESRCH` and a 2 s heartbeat stall. Windows:
  `OpenProcess` + `WaitForSingleObject(h, 0)` or the heartbeat.
- Timestamp clock: `std::chrono::steady_clock` is system-wide on macOS and Windows, so the
  writer's stamp can be subtracted in the reader.
- Because QSharedMemory PosixRealtime works, no raw `shm_open`/`mmap` fallback was needed.
  A thin wrapper (shm_open/mmap on POSIX, CreateFileMappingW on Windows) is still a reasonable
  choice if we want explicit unlink and `O_EXCL` semantics without Qt's key indirection.

Verified snippet (writer side):

```cpp
QSharedMemory shm(QNativeIpcKey(QStringLiteral("/fovea-spike-ring"), QNativeIpcKey::Type::PosixRealtime));
if (!shm.create(12 * 1024 * 1024)) {
    if (shm.error() == QSharedMemory::AlreadyExists) { ::shm_unlink("/fovea-spike-ring"); shm.create(...); }
}
SlotHeader *s = ...;
std::atomic_ref<uint32_t> seq(s->seq);
uint32_t s0 = seq.load(std::memory_order_relaxed);
seq.store(s0 + 1, std::memory_order_relaxed);
std::atomic_thread_fence(std::memory_order_release);
// write payload, frameId, monoNs
seq.store(s0 + 2, std::memory_order_release);
std::atomic_ref<uint32_t>(hdr->latest).store(idx, std::memory_order_release);
```

Reader side:

```cpp
QSharedMemory shm(sameKey);
while (!shm.attach()) QThread::msleep(25);
uint32_t s1 = seq.load(std::memory_order_acquire);
if (s1 & 1u) retry;
memcpy(copy, payload, bytes);
std::atomic_thread_fence(std::memory_order_acquire);
if (seq.load(std::memory_order_relaxed) != s1) retry;
```

## 2. QtHttpServer 6.9

Ground truth: `/opt/homebrew/opt/qt/lib/QtHttpServer.framework/Headers/qhttpserver.h`.
`QHttpServer::listen()` and `afterRequest()` are gone in 6.9 (neither name appears in the
headers). `QHttpServer` is `final`, so `handleRequest` cannot be overridden.

Bind (verified, `http_api 18089 12`):

```cpp
QHttpServer server;
auto *tcp = new QTcpServer(&server);
if (!tcp->listen(QHostAddress::LocalHost, port) || !server.bind(tcp)) { ... }
quint16 actual = tcp->serverPort();      // port 0 gives an ephemeral port
```

Routes with path args, JSON body, JSON responses with status codes (all verified with curl):

```cpp
server.route("/v1/cameras/<arg>", QHttpServerRequest::Method::Get,
             [](const QString &id, const QHttpServerRequest &req) -> QHttpServerResponse {
                 return QHttpServerResponse(QJsonObject{...});                        // 200
                 return QHttpServerResponse(QJsonObject{{"error", "..."}}, QHttpServerResponse::StatusCode::NotFound);
             });
server.route("/v1/cameras", QHttpServerRequest::Method::Post, [](const QHttpServerRequest &req) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(req.body(), &err);              // body is QByteArray
    ...
    return QHttpServerResponse(obj, QHttpServerResponse::StatusCode::Created);         // 201
});
return QHttpServerResponse(QHttpServerResponse::StatusCode::NoContent);                // 204, content-type application/x-empty
```

Handler argument rules (from `QHttpServerRouterViewTraits`): captured `<arg>` values first
(QString, int, qint64, double, QByteArray, QUrl, QUuid, ...), then optionally
`const QHttpServerRequest &` and/or `QHttpServerResponder &` last. Handlers taking a responder
must return void.

Bearer token pre-check: there is no pre-request hook in the public 6.9 API.
`addAfterRequestHandler(context, void(const QHttpServerRequest &, QHttpServerResponse &))`
runs after the route handler produced its response (verified: it adds `x-fovea-core: 0.1` to
every routed response, but NOT to the missing-handler 404), so it cannot short-circuit a
handler's side effects. `QHttpServerRouterRule::matches()` is virtual but can only decline a
match (falls through to 404), not answer 401. The working pattern is a route wrapper:

```cpp
bool bearerOk(const QHttpServerRequest &req) {
    const QByteArray auth = req.headers().value(QHttpHeaders::WellKnownHeader::Authorization).toByteArray();
    return auth.startsWith("Bearer ") && auth.mid(7).trimmed() == kToken;
}

template <typename... Args, typename F>
auto guarded(F fn) {
    return [fn](Args... args, const QHttpServerRequest &req) -> QHttpServerResponse {
        if (!bearerOk(req))
            return QHttpServerResponse(QJsonObject{{"error", "missing or invalid bearer token"}},
                                       QHttpServerResponse::StatusCode::Unauthorized);
        return fn(args..., req);
    };
}

server.route("/v1/cameras/<arg>", QHttpServerRequest::Method::Get,
             guarded<QString>([](const QString &id, const QHttpServerRequest &) { ... }));
server.route("/v1/cameras", QHttpServerRequest::Method::Post,
             guarded<>([](const QHttpServerRequest &req) { ... }));
```

The explicit `Args...` list is required because ViewTraits introspects `operator()` and cannot
see through a generic (`auto...`) lambda.

Missing handler (verified, 404 JSON; note `QJsonDocument` via responder is written indented):

```cpp
server.setMissingHandler(&server, [](const QHttpServerRequest &req, QHttpServerResponder &responder) {
    responder.write(QJsonDocument(QJsonObject{{"error", "no route " + req.url().path()}}),
                    QHttpServerResponder::StatusCode::NotFound);
});
```

curl results: `/v1/health` 200; `/v1/cameras/cam1` without or with a wrong token 401; with
token 200 JSON; unknown id 404 JSON; POST valid JSON 201; POST `{bad json` 400 with
`invalid json: unterminated object`; DELETE 204; `/nope` 404 from the missing handler.
Linking `Qt6::HttpServer` also pulls in `Qt6::WebSockets` (deploy `QtWebSockets` too).
`QHttpServerConfiguration::setRateLimitPerSecond()` exists (not exercised).

## 3. QtSql / QSQLITE

`sql_check` output (verbatim):

```
drivers: QSQLITE
sqlite_version=3.51.0 transactions=1
journal_mode=WAL -> wal
synchronous=NORMAL ok=1
foreign_keys=ON ok=1, foreign_keys=1, busy_timeout=5000
schema created: 5 statements
orphan insert rejected=1 (FOREIGN KEY constraint failed)
after cascade delete: recordings=0 events=0
wal file exists=1
```

- Only `QSQLITE` is built (plugin `/opt/homebrew/opt/qt/share/qt/plugins/sqldrivers/libqsqlite.dylib`,
  found automatically). Bundled SQLite 3.51.0, so `STRICT` tables work.
- `PRAGMA journal_mode=WAL` returns `wal` and creates the `-wal` file. `PRAGMA foreign_keys=ON` is
  per connection and must be issued after every `open()`; there is no connect option for it.
  `db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=5000")` is honored (`PRAGMA busy_timeout` = 5000).
- Schema is compiled by running each `CREATE TABLE` in one `db.transaction()` / `db.commit()`;
  `REFERENCES ... ON DELETE CASCADE` and `ON DELETE SET NULL` behave as expected once FK is on.
- Destroy every `QSqlQuery` and the `QSqlDatabase` handle before `QSqlDatabase::removeDatabase()`
  or Qt logs "connection is still in use". Use one connection name per thread.

```cpp
QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "core");
db.setDatabaseName(path);
db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=5000");
db.open();
QSqlQuery(db).exec("PRAGMA journal_mode=WAL");
QSqlQuery(db).exec("PRAGMA synchronous=NORMAL");
QSqlQuery(db).exec("PRAGMA foreign_keys=ON");
db.transaction(); for (sql : schema) QSqlQuery(db).exec(sql); db.commit();
```

## 4. QProcess::startDetached and QLockFile single instance

`proc_lock` run (verbatim, trimmed paths):

```
launch: startDetached=1 pid=95966
service: pid=95966 holding lock for 6 s
status: lockinfo=1 pid=95966 host=MacBook-Pro.local app=proc_lock acquired=0 error=1
service: already running pid=95966 app=proc_lock error=1    (exit 3)
  PID  PPID   SESS  PGID STAT COMMAND
95966     1      0 95965 S    .../proc_lock --service ... 6
(kill -9 95966)
status: lockinfo=1 pid=95966 host=MacBook-Pro.local app=proc_lock acquired=1 error=0
service: pid=95979 holding lock for 0 s
```

- `QProcess::startDetached(program, args, workDir, &pid)` returns the child pid; the child is
  reparented to PID 1 with its own process group and keeps running after the launcher exits.
- `QLockFile` at `<dir>/fovea-core.lock`: `tryLock(100ms)` fails with `LockFailedError` (1)
  while the service holds it, and `getLockInfo()` yields pid, hostname and the executable name
  (Qt 6 stores the process name by pid, not `applicationName()`). The lock file is the pid file.
- After `kill -9`, the lock file remains but `tryLock` sees the pid is dead and reclaims it
  immediately (stale-time only applies when the pid is alive); the next service starts normally.
- Windows note: `startDetached` for a console-subsystem core may open a console window
  (Qt uses CREATE_NEW_CONSOLE); build the core with `WIN32` subsystem or verify on Windows.

```cpp
QLockFile lock(QDir(dir).filePath("fovea-core.lock"));
lock.setStaleLockTime(std::chrono::seconds(30));
if (!lock.tryLock(std::chrono::milliseconds(100))) { qint64 pid; lock.getLockInfo(&pid, nullptr, nullptr); ... }
qint64 pid = 0;
QProcess::startDetached(QCoreApplication::applicationFilePath(), {"--service", dir}, dir, &pid);
```

## 5. CMake that worked

```cmake
cmake_minimum_required(VERSION 3.22)
project(fovea_qt_spikes LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.9 REQUIRED COMPONENTS Core Network Sql HttpServer Widgets Test)
qt_standard_project_setup()

if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
    get_target_property(_fovea_gl WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
    list(FILTER _fovea_gl EXCLUDE REGEX "AGL")
    set_target_properties(WrapOpenGL::WrapOpenGL PROPERTIES INTERFACE_LINK_LIBRARIES "${_fovea_gl}")
endif()

qt_add_executable(shm_ring shm_ring.cpp)
target_link_libraries(shm_ring PRIVATE Qt6::Core)
qt_add_executable(http_api http_api.cpp)
target_link_libraries(http_api PRIVATE Qt6::Core Qt6::Network Qt6::HttpServer)
qt_add_executable(sql_check sql_check.cpp)
target_link_libraries(sql_check PRIVATE Qt6::Core Qt6::Sql)
qt_add_executable(proc_lock proc_lock.cpp)
target_link_libraries(proc_lock PRIVATE Qt6::Core)
qt_add_executable(tst_smoke tst_smoke.cpp)
target_link_libraries(tst_smoke PRIVATE Qt6::Core Qt6::Widgets Qt6::Test)
```

Configure with `-DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt -G Ninja -DCMAKE_BUILD_TYPE=Release`.
`tst_smoke` (QTEST_MAIN with a QLabel) passes under `QT_QPA_PLATFORM=offscreen`, which proves
Widgets and Test link.

macOS specifics:

- Homebrew Qt is framework based; CMake emits `-F/opt/homebrew/opt/qt/lib`, links
  `QtCore.framework/Versions/A/QtCore` etc. and adds `-Wl,-rpath,/opt/homebrew/opt/qt/lib`,
  so binaries run in place. Compile line: `-std=c++20 -arch arm64 -iframework /opt/homebrew/opt/qt/lib
  -isystem <framework>/Headers -DQT_CORE_LIB ...`.
- Qt 6.9.0's `FindWrapOpenGL.cmake` links `AGL.framework`, which the macOS 26 SDK (Xcode 26.6)
  no longer ships: any Gui-dependent target fails with `ld: framework 'AGL' not found`.
  The `list(FILTER ... EXCLUDE REGEX "AGL")` block above is the consumer-side fix (Core/Network/
  Sql/HttpServer-only targets are unaffected). Newer Qt releases drop AGL, so revisit on upgrade.
- `qt_add_executable` does not create an `.app` bundle unless `MACOSX_BUNDLE` is set; a plain
  Mach-O is fine for the headless core and for dev builds of the console.
- The `slots` identifier is a Qt keyword macro; do not use it as a struct member (or add
  `QT_NO_KEYWORDS`).

Windows MSVC (not run, notes only):

- Same `find_package`/`qt_standard_project_setup` lines with
  `-DCMAKE_PREFIX_PATH=C:/Qt/6.9.0/msvc2022_64`; select "Qt HTTP Server" and "Qt WebSockets"
  in the Qt installer (HttpServer depends on WebSockets).
- Qt's `Qt6::Platform` target already adds `/Zc:__cplusplus /permissive-`; C++20 works with VS 2022.
- Console app: `qt_add_executable(fovea_console WIN32 ...)`; headless core can stay console
  subsystem (see startDetached note). Deploy with
  `qt_generate_deploy_app_script(TARGET fovea_console OUTPUT_SCRIPT s)` + `install(SCRIPT ${s})`
  or `windeployqt`, which copies `platforms/qwindows.dll`, `sqldrivers/qsqlite.dll`,
  `Qt6HttpServer.dll`, `Qt6WebSockets.dll`.
- Shared memory: `QNativeIpcKey::Type::Windows` (`CreateFileMappingW`), plain names are session
  local; `Global\` names need privilege. 12 MB is fine, and the object is freed with the last handle.
- `QLockFile` and `QProcess::startDetached` are supported; the lock's stale detection uses the
  pid plus process name on Windows too.
