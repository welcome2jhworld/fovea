#include "fovea/core/VectorStore.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
#include <cstring>

using namespace fovea::core;

namespace {

constexpr int64_t kDayMs = 86'400'000;
constexpr int64_t kUtc = 1'757'700'000'000;

VectorStore::Record record(int64_t id, int dims, float seed) {
  VectorStore::Record r;
  r.id = id;
  r.utcMs = kUtc + id;
  for (int d = 0; d < dims; ++d) r.vector.push_back(seed + static_cast<float>(d));
  return r;
}

bool sameVector(const float* v, const VectorStore::Record& r) {
  return v && std::memcmp(v, r.vector.constData(), static_cast<size_t>(r.vector.size()) * sizeof(float)) == 0;
}

void appendBytes(const QString& path, const QByteArray& bytes) {
  QFile f(path);
  QVERIFY(f.open(QIODevice::Append));
  QCOMPARE(f.write(bytes), bytes.size());
}

}

class TestVectorStore : public QObject {
  Q_OBJECT
private slots:
  void appendsAndReloads();
  void truncatesPartialRecord();
  void compactsIntoFreshFile();
  void refusesDimsMismatch();
  void readerChecksRecordIdentity();
};

void TestVectorStore::appendsAndReloads() {
  QTemporaryDir dir;
  const QString root = dir.path() + "/index";
  QString file;
  QVector<VectorStore::Record> first{record(1, 4, 0.5f), record(2, 4, 1.5f), record(3, 4, 2.5f)};
  {
    VectorStore store(root);
    file = store.appendFile("v1", "cam", kUtc);
    QVERIFY(file.startsWith("v1/cam/"));
    QVERIFY(file.endsWith(".vec"));
    QCOMPARE(store.appendFile("v1", "cam", kUtc + 1000), file);
    QVERIFY(store.appendFile("v1", "cam", kUtc + kDayMs) != file);
    QVector<int64_t> offsets;
    QString error;
    QVERIFY2(store.append(file, 4, first, &offsets, &error), qPrintable(error));
    QCOMPARE(offsets, (QVector<int64_t>{16, 48, 80}));
    QCOMPARE(QFileInfo(store.absolutePath(file)).size(), 16 + 3 * VectorStore::recordBytes(4));
    QCOMPARE(store.listFiles(), QStringList{file});
    QCOMPARE(VectorStore::versionOf(file), QString("v1"));
  }
  VectorStore reloaded(root);
  QCOMPARE(reloaded.appendFile("v1", "cam", kUtc), file);
  const QVector<VectorStore::Record> second{record(4, 4, 3.5f)};
  QVector<int64_t> offsets;
  QVERIFY(reloaded.append(file, 4, second, &offsets, nullptr));
  QCOMPARE(offsets, QVector<int64_t>{112});
  const VectorStore::Check check = reloaded.repair(file);
  QVERIFY(check.valid);
  QCOMPARE(check.dims, 4);
  QCOMPARE(check.records, 4);
  QCOMPARE(check.truncatedBytes, 0);
  VectorStore::Reader reader;
  QVERIFY(reader.open(reloaded.absolutePath(file), 4));
  QVERIFY(sameVector(reader.vector(16, 1), first[0]));
  QVERIFY(sameVector(reader.vector(80, 3), first[2]));
  QVERIFY(sameVector(reader.vector(112, 4), second[0]));
}

void TestVectorStore::truncatesPartialRecord() {
  QTemporaryDir dir;
  VectorStore store(dir.path());
  const QString file = store.appendFile("v1", "cam", kUtc);
  QVERIFY(store.append(file, 8, {record(1, 8, 0), record(2, 8, 1)}, nullptr, nullptr));
  const int64_t whole = QFileInfo(store.absolutePath(file)).size();
  appendBytes(store.absolutePath(file), QByteArray(21, 'z'));
  VectorStore::Check check = store.repair(file);
  QVERIFY(check.valid);
  QCOMPARE(check.records, 2);
  QCOMPARE(check.truncatedBytes, 21);
  QCOMPARE(QFileInfo(store.absolutePath(file)).size(), whole);

  appendBytes(store.absolutePath(file), QByteArray(7, 'z'));
  QVector<int64_t> offsets;
  QVERIFY(store.append(file, 8, {record(3, 8, 2)}, &offsets, nullptr));
  QCOMPARE(offsets, QVector<int64_t>{whole});
  check = store.repair(file);
  QCOMPARE(check.records, 3);
  QCOMPARE(check.truncatedBytes, 0);
  VectorStore::Reader reader;
  QVERIFY(reader.open(store.absolutePath(file), 8));
  QVERIFY(sameVector(reader.vector(whole, 3), record(3, 8, 2)));

  QFile headerOnly(store.absolutePath("v1/cam/short.vec"));
  QVERIFY(headerOnly.open(QIODevice::WriteOnly));
  headerOnly.write(QByteArray(10, 'x'));
  headerOnly.close();
  QVERIFY(!store.repair("v1/cam/short.vec").valid);
}

void TestVectorStore::compactsIntoFreshFile() {
  QTemporaryDir dir;
  VectorStore store(dir.path());
  const QString file = store.appendFile("v1", "cam", kUtc);
  QVector<VectorStore::Record> records;
  for (int i = 1; i <= 6; ++i) records.push_back(record(i, 3, static_cast<float>(i * 10)));
  QVector<int64_t> offsets;
  QVERIFY(store.append(file, 3, records, &offsets, nullptr));

  store.seal(file);
  const QString next = store.appendFile("v1", "cam", kUtc);
  QVERIFY(next != file);
  const QString target = store.freshName(file);
  QVERIFY(target != file && target != next);
  QVERIFY(store.freshName(file) != target);

  const QVector<int64_t> keep{offsets[1], offsets[4]};
  QVector<int64_t> moved;
  QString error;
  QVERIFY2(VectorStore::copyRecords(store.root(), file, target, 3, keep, &moved, &error), qPrintable(error));
  QCOMPARE(moved, (QVector<int64_t>{16, 16 + VectorStore::recordBytes(3)}));
  QVERIFY(!VectorStore::copyRecords(store.root(), file, target, 3, keep, &moved, &error));
  VectorStore::Reader reader;
  QVERIFY(reader.open(store.absolutePath(target), 3));
  QVERIFY(sameVector(reader.vector(moved[0], 2), records[1]));
  QVERIFY(sameVector(reader.vector(moved[1], 5), records[4]));
  QCOMPARE(store.repair(target).records, 2);

  QVERIFY(!VectorStore::copyRecords(store.root(), file, store.freshName(file), 3, {offsets[5] + 1000}, &moved, &error));
  QVERIFY(store.remove(file));
  QVERIFY(!QFile::exists(store.absolutePath(file)));
  QVERIFY(store.append(next, 3, {record(7, 3, 70)}, nullptr, nullptr));
  QVERIFY(store.removeVersion("v1"));
  QVERIFY(store.listFiles().isEmpty());
}

void TestVectorStore::refusesDimsMismatch() {
  QTemporaryDir dir;
  VectorStore store(dir.path());
  const QString file = store.appendFile("v1", "cam", kUtc);
  QVERIFY(store.append(file, 4, {record(1, 4, 0)}, nullptr, nullptr));
  QString error;
  QVERIFY(!store.append(file, 8, {record(2, 8, 0)}, nullptr, &error));
  QVERIFY(error.contains("dims mismatch"));
  QVERIFY(!store.append(file, 4, {record(2, 5, 0)}, nullptr, &error));
  QCOMPARE(store.repair(file).records, 1);
  VectorStore::Reader wrong;
  QVERIFY(!wrong.open(store.absolutePath(file), 8));
  QVERIFY(!wrong.vector(16, 1));
  QVector<int64_t> moved;
  QVERIFY(!VectorStore::copyRecords(store.root(), file, store.freshName(file), 8, {16}, &moved, &error));

  QFile f(store.absolutePath(file));
  QVERIFY(f.open(QIODevice::ReadWrite));
  f.write("XXXX");
  f.close();
  const VectorStore::Check check = store.repair(file);
  QVERIFY(!check.valid);
  QCOMPARE(check.error, QString("not a vector file"));
  QVERIFY(!store.append(file, 4, {record(3, 4, 0)}, nullptr, &error));
}

void TestVectorStore::readerChecksRecordIdentity() {
  QTemporaryDir dir;
  VectorStore store(dir.path());
  const QString file = store.appendFile("v1", "cam", kUtc);
  QVERIFY(store.append(file, 2, {record(10, 2, 0), record(11, 2, 5)}, nullptr, nullptr));
  VectorStore::Reader reader;
  QVERIFY(reader.open(store.absolutePath(file), 2));
  QVERIFY(reader.vector(16, 10));
  QVERIFY(!reader.vector(16, 11));
  QVERIFY(!reader.vector(17, 10));
  QVERIFY(!reader.vector(16 + 2 * VectorStore::recordBytes(2), 12));
  QVERIFY(!reader.vector(0, 10));
  VectorStore::Reader missing;
  QVERIFY(!missing.open(store.absolutePath("v1/cam/none.vec"), 2));
}

QTEST_GUILESS_MAIN(TestVectorStore)
#include "test_vector_store.moc"
