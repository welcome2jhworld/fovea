#include "fovea/core/VectorStore.h"
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QTimeZone>
#include <bit>
#include <cstring>
#include <utility>

namespace fovea::core {
namespace {

static_assert(std::endian::native == std::endian::little, "vector files are written in host byte order");

const QString kSuffix = QStringLiteral(".vec");
constexpr qsizetype kCopyChunkRecords = 4096;

struct Header {
  uint32_t magic = 0;
  uint32_t version = 0;
  uint32_t dims = 0;
  uint32_t dtype = 0;
};

QByteArray headerBytes(int dims) {
  const Header h{VectorStore::kMagic, VectorStore::kFormatVersion, static_cast<uint32_t>(dims), VectorStore::kDtypeFloat32};
  return QByteArray(reinterpret_cast<const char*>(&h), sizeof(Header));
}

QString headerProblem(const Header& h) {
  if (h.magic != VectorStore::kMagic) return QStringLiteral("not a vector file");
  if (h.version != VectorStore::kFormatVersion) return QStringLiteral("unsupported vector file version %1").arg(h.version);
  if (h.dtype != VectorStore::kDtypeFloat32) return QStringLiteral("unsupported dtype %1").arg(h.dtype);
  if (h.dims == 0 || h.dims > 65536) return QStringLiteral("invalid dims %1").arg(h.dims);
  return {};
}

bool readHeader(QFile& f, Header* h) {
  if (!f.seek(0)) return false;
  return f.read(reinterpret_cast<char*>(h), sizeof(Header)) == sizeof(Header);
}

// "<day>" or "<day>-<n>" without the suffix, as the day and n.
std::pair<QString, int> splitName(const QString& relFile) {
  const QString base = QFileInfo(relFile).completeBaseName();
  const qsizetype dash = base.indexOf(QLatin1Char('-'));
  if (dash < 0) return {base, 0};
  return {base.left(dash), base.mid(dash + 1).toInt()};
}

QString dayKey(const QString& relFile) { return QFileInfo(relFile).path() + QLatin1Char('/') + splitName(relFile).first; }

QString nameFor(const QString& dir, const QString& day, int n) {
  return dir + QLatin1Char('/') + (n == 0 ? day : QStringLiteral("%1-%2").arg(day).arg(n)) + kSuffix;
}

bool failWith(QString* error, const QString& message) {
  if (error) *error = message;
  return false;
}

}

VectorStore::Reader::~Reader() {
  if (map_) file_.unmap(map_);
}

bool VectorStore::Reader::open(const QString& path, int dims) {
  file_.setFileName(path);
  if (!file_.open(QIODevice::ReadOnly)) return false;
  size_ = file_.size();
  if (size_ < kHeaderBytes) return false;
  map_ = file_.map(0, size_);
  if (!map_) return false;
  Header h;
  std::memcpy(&h, map_, sizeof(Header));
  if (!headerProblem(h).isEmpty() || h.dims != static_cast<uint32_t>(dims)) return false;
  dims_ = dims;
  return true;
}

const float* VectorStore::Reader::vector(int64_t offset, int64_t recordId) const {
  if (!map_ || dims_ <= 0) return nullptr;
  const int64_t rec = recordBytes(dims_);
  if (offset < kHeaderBytes || (offset - kHeaderBytes) % rec != 0 || offset + rec > size_) return nullptr;
  uint64_t id = 0;
  std::memcpy(&id, map_ + offset, sizeof(id));
  if (id != static_cast<uint64_t>(recordId)) return nullptr;
  return reinterpret_cast<const float*>(static_cast<void*>(map_ + offset + 16));
}

VectorStore::VectorStore(QString rootDir) : root_(std::move(rootDir)) {}

QString VectorStore::absolutePath(const QString& relFile) const { return root_ + QLatin1Char('/') + relFile; }

QString VectorStore::versionOf(const QString& relFile) { return relFile.section(QLatin1Char('/'), 0, 0); }

QString VectorStore::appendFile(const QString& version, const QString& cameraId, int64_t utcMs) {
  const QString day = QDateTime::fromMSecsSinceEpoch(utcMs, QTimeZone::UTC).toString(QStringLiteral("yyyyMMdd"));
  const QString dir = version + QLatin1Char('/') + cameraId;
  const QString key = dir + QLatin1Char('/') + day;
  if (const auto it = current_.constFind(key); it != current_.constEnd()) return it.value();
  int best = 0;
  for (const QString& name : QDir(absolutePath(dir)).entryList({day + QStringLiteral("*") + kSuffix}, QDir::Files)) {
    const auto [fileDay, n] = splitName(name);
    if (fileDay == day) best = std::max(best, n);
  }
  const QString file = nameFor(dir, day, best);
  current_.insert(key, file);
  issued_.insert(file);
  return file;
}

void VectorStore::seal(const QString& relFile) {
  const QString key = dayKey(relFile);
  const auto it = current_.constFind(key);
  if (it == current_.constEnd() || it.value() == relFile) current_.insert(key, freshName(relFile));
}

QString VectorStore::freshName(const QString& relFile) {
  const QString dir = QFileInfo(relFile).path();
  const QString day = splitName(relFile).first;
  int n = 0;
  for (const QString& name : QDir(absolutePath(dir)).entryList({day + QStringLiteral("*") + kSuffix}, QDir::Files)) {
    const auto [fileDay, index] = splitName(name);
    if (fileDay == day) n = std::max(n, index);
  }
  for (const QString& issued : std::as_const(issued_))
    if (dayKey(issued) == dir + QLatin1Char('/') + day) n = std::max(n, splitName(issued).second);
  const QString file = nameFor(dir, day, n + 1);
  issued_.insert(file);
  return file;
}

QStringList VectorStore::listFiles() const {
  QStringList out;
  const QDir root(root_);
  QDirIterator it(root_, {QStringLiteral("*") + kSuffix}, QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext()) out.push_back(root.relativeFilePath(it.next()));
  out.sort();
  return out;
}

bool VectorStore::append(const QString& relFile, int dims, const QVector<Record>& records, QVector<int64_t>* offsets,
                         QString* error) {
  if (dims <= 0) return failWith(error, QStringLiteral("invalid dims %1").arg(dims));
  const QString path = absolutePath(relFile);
  if (!QDir().mkpath(QFileInfo(path).path())) return failWith(error, QStringLiteral("cannot create the vector directory"));
  QFile f(path);
  if (!f.open(QIODevice::ReadWrite)) return failWith(error, QStringLiteral("cannot open %1: %2").arg(relFile, f.errorString()));
  const int64_t rec = recordBytes(dims);
  int64_t end = f.size();
  if (end < kHeaderBytes) {
    if (!f.resize(0) || f.write(headerBytes(dims)) != kHeaderBytes) return failWith(error, QStringLiteral("cannot write the header"));
    end = kHeaderBytes;
  } else {
    Header h;
    if (!readHeader(f, &h)) return failWith(error, QStringLiteral("cannot read the header"));
    if (const QString problem = headerProblem(h); !problem.isEmpty()) return failWith(error, problem);
    if (h.dims != static_cast<uint32_t>(dims))
      return failWith(error, QStringLiteral("dims mismatch: file has %1, records have %2").arg(h.dims).arg(dims));
    const int64_t aligned = kHeaderBytes + (end - kHeaderBytes) / rec * rec;
    if (aligned != end && !f.resize(aligned)) return failWith(error, QStringLiteral("cannot truncate a partial record"));
    end = aligned;
  }
  QByteArray buffer(static_cast<qsizetype>(rec * records.size()), Qt::Uninitialized);
  QVector<int64_t> written;
  written.reserve(records.size());
  char* out = buffer.data();
  for (const Record& r : records) {
    if (r.vector.size() != dims)
      return failWith(error, QStringLiteral("record %1 has %2 values, expected %3").arg(r.id).arg(r.vector.size()).arg(dims));
    const uint64_t id = static_cast<uint64_t>(r.id);
    std::memcpy(out, &id, sizeof(id));
    std::memcpy(out + 8, &r.utcMs, sizeof(r.utcMs));
    std::memcpy(out + 16, r.vector.constData(), static_cast<size_t>(dims) * sizeof(float));
    written.push_back(end + static_cast<int64_t>(written.size()) * rec);
    out += rec;
  }
  if (!f.seek(end) || f.write(buffer) != buffer.size() || !f.flush())
    return failWith(error, QStringLiteral("cannot write %1: %2").arg(relFile, f.errorString()));
  if (offsets) *offsets = written;
  return true;
}

VectorStore::Check VectorStore::repair(const QString& relFile) {
  Check c;
  QFile f(absolutePath(relFile));
  if (!f.open(QIODevice::ReadWrite)) {
    c.error = f.errorString();
    return c;
  }
  Header h;
  if (f.size() < kHeaderBytes || !readHeader(f, &h)) {
    c.error = QStringLiteral("no header");
    return c;
  }
  if (c.error = headerProblem(h); !c.error.isEmpty()) return c;
  c.dims = static_cast<int>(h.dims);
  const int64_t rec = recordBytes(c.dims);
  const int64_t size = f.size();
  c.records = (size - kHeaderBytes) / rec;
  const int64_t aligned = kHeaderBytes + c.records * rec;
  if (aligned != size) {
    if (!f.resize(aligned)) {
      c.error = QStringLiteral("cannot truncate a partial record");
      return c;
    }
    c.truncatedBytes = size - aligned;
  }
  c.valid = true;
  return c;
}

bool VectorStore::copyRecords(const QString& root, const QString& relFrom, const QString& relTo, int dims,
                              const QVector<int64_t>& offsets, QVector<int64_t>* newOffsets, QString* error) {
  const QString target = root + QLatin1Char('/') + relTo;
  if (QFileInfo::exists(target)) return failWith(error, QStringLiteral("%1 already exists").arg(relTo));
  QFile in(root + QLatin1Char('/') + relFrom);
  if (!in.open(QIODevice::ReadOnly)) return failWith(error, QStringLiteral("cannot open %1").arg(relFrom));
  Header h;
  if (!readHeader(in, &h) || !headerProblem(h).isEmpty() || h.dims != static_cast<uint32_t>(dims))
    return failWith(error, QStringLiteral("%1 has no valid header for %2 dims").arg(relFrom).arg(dims));
  const int64_t rec = recordBytes(dims);
  if (!QDir().mkpath(QFileInfo(target).path())) return failWith(error, QStringLiteral("cannot create the vector directory"));
  QFile out(target);
  if (!out.open(QIODevice::NewOnly | QIODevice::WriteOnly) || out.write(headerBytes(dims)) != kHeaderBytes)
    return failWith(error, QStringLiteral("cannot create %1").arg(relTo));
  QVector<int64_t> moved;
  moved.reserve(offsets.size());
  QByteArray chunk;
  chunk.reserve(static_cast<qsizetype>(rec) * kCopyChunkRecords);
  const auto abandon = [&out, error](const QString& message) {
    out.close();
    out.remove();
    return failWith(error, message);
  };
  for (const int64_t offset : offsets) {
    QByteArray bytes;
    if (in.seek(offset)) bytes = in.read(rec);
    if (bytes.size() != rec) return abandon(QStringLiteral("record at %1 of %2 is missing").arg(offset).arg(relFrom));
    moved.push_back(kHeaderBytes + static_cast<int64_t>(moved.size()) * rec);
    chunk.append(bytes);
    if (chunk.size() >= static_cast<qsizetype>(rec) * kCopyChunkRecords) {
      if (out.write(chunk) != chunk.size()) return abandon(QStringLiteral("cannot write %1").arg(relTo));
      chunk.clear();
    }
  }
  if ((!chunk.isEmpty() && out.write(chunk) != chunk.size()) || !out.flush())
    return abandon(QStringLiteral("cannot write %1").arg(relTo));
  if (newOffsets) *newOffsets = moved;
  return true;
}

bool VectorStore::remove(const QString& relFile) {
  const QString key = dayKey(relFile);
  if (current_.value(key) == relFile) current_.remove(key);
  const QString path = absolutePath(relFile);
  return !QFileInfo::exists(path) || QFile::remove(path);
}

bool VectorStore::removeVersion(const QString& version) {
  if (version.isEmpty() || version.contains(QLatin1Char('/')) || version.startsWith(QLatin1Char('.'))) return false;
  const QString prefix = version + QLatin1Char('/');
  for (auto it = current_.begin(); it != current_.end();) {
    if (it.key().startsWith(prefix)) it = current_.erase(it);
    else ++it;
  }
  return QDir(absolutePath(version)).removeRecursively();
}

}
