#pragma once
#include <QFile>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <cstdint>

namespace fovea::core {

// Append-only vector files under a root directory:
// <root>/<index_version>/<camera_id>/<yyyymmdd>[-<n>].vec. A file is a 16-byte
// header {magic "FVEC", format version, dims, dtype} followed by fixed-size
// little-endian records {record_id uint64, utc_ms int64, vector float32[dims]}.
// Paths passed in and out are relative to the root. SQLite keeps which record
// lives at which offset: records no row points at (a crash between the append
// and the commit) are ignored and dropped by compaction. Everything but Reader
// and copyRecords belongs to one thread.
class VectorStore {
public:
  static constexpr uint32_t kMagic = 0x43455646;
  static constexpr uint32_t kFormatVersion = 1;
  static constexpr uint32_t kDtypeFloat32 = 1;
  static constexpr int64_t kHeaderBytes = 16;

  struct Record {
    int64_t id = 0;
    int64_t utcMs = 0;
    QVector<float> vector;
  };

  struct Check {
    bool valid = false;
    int dims = 0;
    int64_t records = 0;
    int64_t truncatedBytes = 0;
    QString error;
  };

  // A read-only mapping of one file; usable from any thread.
  class Reader {
  public:
    Reader() = default;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    ~Reader();
    // False when the file is missing, its header is invalid or its dims differ.
    bool open(const QString& path, int dims);
    // The vector at offset, when a whole record carrying recordId is there.
    const float* vector(int64_t offset, int64_t recordId) const;

  private:
    QFile file_;
    uchar* map_ = nullptr;
    int64_t size_ = 0;
    int dims_ = 0;
  };

  explicit VectorStore(QString rootDir);

  const QString& root() const { return root_; }
  QString absolutePath(const QString& relFile) const;
  static int64_t recordBytes(int dims) { return 16 + 4 * static_cast<int64_t>(dims); }
  static QString versionOf(const QString& relFile);

  // The file new records of the version, camera and UTC day are appended to.
  QString appendFile(const QString& version, const QString& cameraId, int64_t utcMs);
  // Later appends for relFile's day go to a new file, so relFile can be copied elsewhere.
  void seal(const QString& relFile);
  // A name for the day of relFile that no file has and no earlier call returned.
  QString freshName(const QString& relFile);
  QStringList listFiles() const;

  // Appends after the last whole record (a trailing partial record is cut off
  // first), creating the file with its header. Refuses a file with an invalid
  // header or other dims. offsets receives each record's offset.
  bool append(const QString& relFile, int dims, const QVector<Record>& records, QVector<int64_t>* offsets, QString* error);
  // Validates the header and truncates a trailing partial record.
  Check repair(const QString& relFile);
  // Copies the records at offsets, in that order, into relTo, which must not
  // exist; newOffsets receives their offsets there. Reads relFrom only.
  static bool copyRecords(const QString& root, const QString& relFrom, const QString& relTo, int dims,
                          const QVector<int64_t>& offsets, QVector<int64_t>* newOffsets, QString* error);
  bool remove(const QString& relFile);
  // Removes <root>/<version> with everything in it.
  bool removeVersion(const QString& version);

private:
  QString root_;
  QHash<QString, QString> current_;
  QSet<QString> issued_;
};

}
