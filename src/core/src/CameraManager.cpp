#include "fovea/core/CameraManager.h"
#include "fovea/Clock.h"
#include "fovea/Ids.h"
#include "fovea/Redact.h"
#include "fovea/core/CameraPipeline.h"
#include "fovea/core/Store.h"
#include <QJsonArray>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <chrono>
#include <map>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <QFile>
#endif

namespace fovea::core {
namespace {

constexpr std::chrono::milliseconds kStopAllWait{10000};

int64_t processRssBytes() {
#if defined(__APPLE__)
  mach_task_basic_info info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return 0;
  return static_cast<int64_t>(info.resident_size);
#elif defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pmc{};
  if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
  return static_cast<int64_t>(pmc.WorkingSetSize);
#else
  QFile statm(QStringLiteral("/proc/self/statm"));
  if (!statm.open(QIODevice::ReadOnly)) return 0;
  const QList<QByteArray> fields = statm.readAll().simplified().split(' ');
  if (fields.size() < 2) return 0;
  return fields[1].toLongLong() * 4096;
#endif
}

QString validateCamera(const Camera& c) {
  if (c.name.trimmed().isEmpty()) return QStringLiteral("name is required");
  if (c.mainUrl.trimmed().isEmpty()) return QStringLiteral("main_url is required");
  if (c.kind != QLatin1String("rtsp") && c.kind != QLatin1String("file")) return QStringLiteral("kind must be rtsp or file");
  if (c.transport != QLatin1String("tcp") && c.transport != QLatin1String("udp")) return QStringLiteral("transport must be tcp or udp");
  if (c.jitterMs < 100 || c.jitterMs > 5000) return QStringLiteral("jitter_ms must be between 100 and 5000");
  if (c.segmentSeconds < 5 || c.segmentSeconds > 600) return QStringLiteral("segment_seconds must be between 5 and 600");
  if (c.timeoutMs <= 0) return QStringLiteral("timeout_ms must be positive");
  if (c.kind == QLatin1String("rtsp")) {
    const QUrl u(c.mainUrl);
    if (!u.isValid() || u.host().isEmpty() || !u.scheme().startsWith(QLatin1String("rtsp")))
      return QStringLiteral("main_url must be an rtsp:// URL");
  }
  return {};
}

std::optional<Credentials> takeUserInfo(QString& url) {
  QUrl u(url);
  if (!u.isValid() || (u.userName().isEmpty() && u.password().isEmpty())) return std::nullopt;
  Credentials creds{u.userName(QUrl::FullyDecoded), u.password(QUrl::FullyDecoded)};
  u.setUserInfo(QString());
  url = u.toString(QUrl::FullyEncoded);
  return creds;
}

struct ResolvedCredentials {
  std::optional<Credentials> credentials;
  QString error;
};

// Credentials typed into main_url or sub_url move to the secret store so no
// URL that is stored, logged or returned carries them. Explicit credentials
// win over main_url's. The store keeps one pair per camera, so sub_url may
// only repeat the camera's credentials (explicit, main_url or already stored).
ResolvedCredentials resolveCredentials(Camera& c, const std::optional<Credentials>& explicitCreds,
                                       const std::optional<Credentials>& storedCreds) {
  ResolvedCredentials out{explicitCreds, {}};
  const std::optional<Credentials> fromMain = c.kind == QLatin1String("rtsp") ? takeUserInfo(c.mainUrl) : std::nullopt;
  if (!out.credentials) out.credentials = fromMain;
  const std::optional<Credentials> fromSub = takeUserInfo(c.subUrl);
  if (!fromSub) return out;
  const std::optional<Credentials>& current = out.credentials ? out.credentials : storedCreds;
  if (!current) out.credentials = fromSub;
  else if (current->username != fromSub->username || current->password != fromSub->password)
    out.error = QStringLiteral("sub_url credentials must match the camera credentials");
  return out;
}

bool connectionFieldsChanged(const Camera& a, const Camera& b) {
  return a.kind != b.kind || a.mainUrl != b.mainUrl || a.transport != b.transport || a.timeoutMs != b.timeoutMs ||
         a.jitterMs != b.jitterMs || a.segmentSeconds != b.segmentSeconds || a.recordEnabled != b.recordEnabled;
}

}

struct CameraManager::Impl {
  Impl(Store& s, SecretStore& sec, CameraManager* owner) : store(s), secrets(sec), self(owner) {}

  Store& store;
  SecretStore& secrets;
  CameraManager* self;
  std::vector<Camera> cameras;
  std::map<QString, std::unique_ptr<CameraPipeline>> pipelines;
  // Stopped pipelines still finalizing their recordings; deleted once idle.
  std::map<QString, std::unique_ptr<CameraPipeline>> stopping;
  std::function<void()> allStopped;
  bool started = false;

  Camera* find(const QString& id) {
    for (Camera& c : cameras)
      if (c.id == id) return &c;
    return nullptr;
  }

  bool codeInUse(const QString& code, const QString& exceptId) const {
    for (const Camera& c : cameras)
      if (c.code == code && c.id != exceptId) return true;
    return false;
  }

  QString nextCode() const {
    for (int n = 1; n < 10000; ++n) {
      const QString code = QStringLiteral("CAM-%1").arg(n, 2, 10, QLatin1Char('0'));
      if (!codeInUse(code, QString())) return code;
    }
    return QStringLiteral("CAM-") + newId().left(8);
  }

  // A pipeline that is still finalizing is reused, so its next session opens
  // only after the previous one has been torn down.
  void startPipeline(const Camera& c) {
    if (pipelines.count(c.id)) return;
    std::unique_ptr<CameraPipeline> pipeline;
    if (const auto it = stopping.find(c.id); it != stopping.end()) {
      pipeline = std::move(it->second);
      stopping.erase(it);
      pipeline->reconfigure(c, secrets.get(c.id));
    } else {
      pipeline = std::make_unique<CameraPipeline>(c, secrets.get(c.id), store, self->config(), self);
      QObject::connect(pipeline.get(), &CameraPipeline::statusChanged, self, &CameraManager::statusChanged);
      QObject::connect(pipeline.get(), &CameraPipeline::idleReached, self, [this](const QString& id) { reap(id); },
                       Qt::QueuedConnection);
    }
    pipeline->start();
    pipelines.emplace(c.id, std::move(pipeline));
  }

  void stopPipeline(const QString& id, const QString& reason) {
    const auto it = pipelines.find(id);
    if (it == pipelines.end()) return;
    std::unique_ptr<CameraPipeline> pipeline = std::move(it->second);
    pipelines.erase(it);
    pipeline->stop(reason);
    if (!pipeline->idle()) stopping.emplace(id, std::move(pipeline));
  }

  void reap(const QString& id) {
    const auto it = stopping.find(id);
    if (it == stopping.end() || !it->second->idle()) return;
    stopping.erase(it);
    if (stopping.empty() && allStopped) std::exchange(allStopped, nullptr)();
  }
};

CameraManager::CameraManager(Store& store, SecretStore& secrets, CoreConfig config, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(store, secrets, this)), config_(std::move(config)) {}

CameraManager::~CameraManager() = default;

void CameraManager::start() {
  const QVector<Camera> loaded = impl_->store.listCameras();
  impl_->cameras.assign(loaded.begin(), loaded.end());
  impl_->started = true;
  int enabled = 0;
  for (const Camera& c : impl_->cameras) {
    if (!c.enabled) continue;
    impl_->startPipeline(c);
    ++enabled;
  }
  qInfo("camera manager: %zu cameras, %d enabled", impl_->cameras.size(), enabled);
}

void CameraManager::stopAll(std::function<void()> done) {
  impl_->started = false;
  QStringList ids;
  for (const auto& [id, pipeline] : impl_->pipelines) ids.push_back(id);
  for (const QString& id : ids) impl_->stopPipeline(id, QStringLiteral("shutdown"));
  if (impl_->stopping.empty()) {
    done();
    return;
  }
  impl_->allStopped = std::move(done);
  QTimer::singleShot(kStopAllWait, this, [this] {
    const auto pending = std::exchange(impl_->allStopped, nullptr);
    if (!pending) return;
    qWarning("camera manager: %zu cameras still finalizing, stopping anyway", impl_->stopping.size());
    pending();
  });
}

QVector<Camera> CameraManager::cameras() const {
  QVector<Camera> out;
  out.reserve(static_cast<qsizetype>(impl_->cameras.size()));
  for (const Camera& c : impl_->cameras) out.push_back(c);
  return out;
}

std::optional<Camera> CameraManager::camera(const QString& id) const {
  const Camera* c = impl_->find(id);
  if (!c) return std::nullopt;
  return *c;
}

QVector<CameraStatus> CameraManager::statuses() const {
  QVector<CameraStatus> out;
  for (const Camera& c : impl_->cameras)
    if (const auto s = status(c.id)) out.push_back(*s);
  return out;
}

std::optional<CameraStatus> CameraManager::status(const QString& id) const {
  const Camera* c = impl_->find(id);
  if (!c) return std::nullopt;
  const auto it = impl_->pipelines.find(id);
  if (it != impl_->pipelines.end()) return it->second->status();
  CameraStatus s;
  s.cameraId = id;
  s.state = QStringLiteral("disabled");
  s.sinceUtcMs = c->updatedUtcMs;
  s.recording = QStringLiteral("disabled");
  return s;
}

std::optional<Camera> CameraManager::createCamera(Camera c, const std::optional<Credentials>& creds, QString* error) {
  c.id = newId();
  c.name = c.name.trimmed();
  c.mainUrl = c.mainUrl.trimmed();
  c.subUrl = c.subUrl.trimmed();
  const ResolvedCredentials resolved = resolveCredentials(c, creds, std::nullopt);
  const std::optional<Credentials>& stored = resolved.credentials;
  if (const QString err = resolved.error.isEmpty() ? validateCamera(c) : resolved.error; !err.isEmpty()) {
    if (error) *error = err;
    return std::nullopt;
  }
  c.code = c.code.trimmed();
  if (c.code.isEmpty()) c.code = impl_->nextCode();
  else if (impl_->codeInUse(c.code, QString())) {
    if (error) *error = QStringLiteral("code %1 is already used").arg(c.code);
    return std::nullopt;
  }
  c.createdUtcMs = utcNowMs();
  c.updatedUtcMs = c.createdUtcMs;
  if (!impl_->store.insertCamera(c)) {
    if (error) *error = impl_->store.lastError();
    return std::nullopt;
  }
  if (stored && !stored->username.isEmpty()) impl_->secrets.set(c.id, *stored);
  impl_->cameras.push_back(c);
  qInfo("camera %s created (%s, %s)", qPrintable(c.id), qPrintable(c.code), qPrintable(redactUrl(c.mainUrl)));
  if (c.enabled && impl_->started) impl_->startPipeline(c);
  emit statusChanged(c.id);
  return c;
}

bool CameraManager::updateCamera(const Camera& update, const std::optional<Credentials>& creds, QString* error) {
  Camera* existing = impl_->find(update.id);
  if (!existing) {
    if (error) *error = QStringLiteral("no such camera");
    return false;
  }
  Camera c = update;
  c.name = c.name.trimmed();
  c.mainUrl = c.mainUrl.trimmed();
  c.subUrl = c.subUrl.trimmed();
  const ResolvedCredentials resolved = resolveCredentials(c, creds, impl_->secrets.get(c.id));
  const std::optional<Credentials>& stored = resolved.credentials;
  if (const QString err = resolved.error.isEmpty() ? validateCamera(c) : resolved.error; !err.isEmpty()) {
    if (error) *error = err;
    return false;
  }
  c.code = c.code.trimmed();
  if (c.code.isEmpty()) c.code = existing->code.isEmpty() ? impl_->nextCode() : existing->code;
  if (impl_->codeInUse(c.code, c.id)) {
    if (error) *error = QStringLiteral("code %1 is already used").arg(c.code);
    return false;
  }
  c.createdUtcMs = existing->createdUtcMs;
  c.updatedUtcMs = utcNowMs();
  if (!impl_->store.updateCamera(c)) {
    if (error) *error = impl_->store.lastError().isEmpty() ? QStringLiteral("update failed") : impl_->store.lastError();
    return false;
  }
  bool credentialsChanged = false;
  if (stored) {
    credentialsChanged = true;
    if (stored->username.isEmpty()) impl_->secrets.remove(c.id);
    else impl_->secrets.set(c.id, *stored);
  }
  const bool restart = connectionFieldsChanged(*existing, c) || credentialsChanged;
  const bool wasEnabled = existing->enabled;
  *existing = c;

  auto it = impl_->pipelines.find(c.id);
  if (!c.enabled) {
    impl_->stopPipeline(c.id, QStringLiteral("stopped"));
  } else if (it == impl_->pipelines.end()) {
    if (impl_->started) impl_->startPipeline(c);
  } else if (restart || !wasEnabled) {
    it->second->reconfigure(c, impl_->secrets.get(c.id));
  } else {
    it->second->setCamera(c);
  }
  qInfo("camera %s updated%s", qPrintable(c.id), restart ? " (pipeline restarted)" : "");
  emit statusChanged(c.id);
  return true;
}

bool CameraManager::deleteCamera(const QString& id, QString* error) {
  if (!impl_->find(id)) {
    if (error) *error = QStringLiteral("no such camera");
    return false;
  }
  impl_->stopPipeline(id, QStringLiteral("stopped"));
  if (!impl_->store.softDeleteCamera(id, utcNowMs())) {
    if (error) *error = QStringLiteral("delete failed");
    return false;
  }
  impl_->secrets.remove(id);
  auto& cams = impl_->cameras;
  cams.erase(std::remove_if(cams.begin(), cams.end(), [&](const Camera& c) { return c.id == id; }), cams.end());
  qInfo("camera %s deleted", qPrintable(id));
  emit statusChanged(id);
  return true;
}

bool CameraManager::setEnabled(const QString& id, bool enabled, QString* error) {
  Camera* c = impl_->find(id);
  if (!c) {
    if (error) *error = QStringLiteral("no such camera");
    return false;
  }
  if (c->enabled != enabled) {
    c->enabled = enabled;
    c->updatedUtcMs = utcNowMs();
    impl_->store.updateCamera(*c);
  }
  if (enabled) {
    if (impl_->started) impl_->startPipeline(*c);
  } else {
    impl_->stopPipeline(id, QStringLiteral("stopped"));
  }
  emit statusChanged(id);
  return true;
}

void CameraManager::testConnection(const Camera& camera, const std::optional<Credentials>& creds,
                                   std::function<void(ConnectionTest)> done) {
  Camera c = camera;
  c.mainUrl = c.mainUrl.trimmed();
  c.subUrl = c.subUrl.trimmed();
  const std::optional<Credentials> storedCreds = c.id.isEmpty() ? std::nullopt : impl_->secrets.get(c.id);
  const ResolvedCredentials resolved = resolveCredentials(c, creds, storedCreds);
  const std::optional<Credentials> used = resolved.credentials ? resolved.credentials : storedCreds;
  if (c.name.isEmpty()) c.name = QStringLiteral("probe");
  if (const QString err = resolved.error.isEmpty() ? validateCamera(c) : resolved.error; !err.isEmpty()) {
    ConnectionTest result;
    result.error = err;
    QMetaObject::invokeMethod(this, [done = std::move(done), result] { done(result); }, Qt::QueuedConnection);
    return;
  }
  qInfo("camera test: probing %s", qPrintable(redactUrl(c.mainUrl)));
  auto* probe = new ConnectionProbe(c, used, this);
  probe->run(std::move(done));
}

QJsonObject CameraManager::metrics() const {
  QJsonArray cams;
  uint64_t ringBytes = 0;
  for (const Camera& c : impl_->cameras) {
    const auto it = impl_->pipelines.find(c.id);
    if (it == impl_->pipelines.end()) {
      cams.push_back(QJsonObject{{"camera_id", c.id}, {"state", "disabled"}});
      continue;
    }
    cams.push_back(it->second->metrics());
    ringBytes += it->second->ringBytes();
  }
  return QJsonObject{{"cameras", cams},
                     {"process", QJsonObject{{"rss_bytes", static_cast<double>(processRssBytes())},
                                             {"ring_bytes", static_cast<double>(ringBytes)},
                                             {"pipelines", static_cast<int>(impl_->pipelines.size())}}}};
}

}
