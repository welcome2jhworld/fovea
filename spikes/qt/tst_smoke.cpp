#include <QtTest>
#include <QApplication>
#include <QLabel>

class SmokeTest : public QObject
{
    Q_OBJECT
private slots:
    void widgetsAndTestLink()
    {
        QLabel label(QStringLiteral("fovea"));
        QCOMPARE(label.text(), QStringLiteral("fovea"));
        QVERIFY(qApp != nullptr);
    }
};

QTEST_MAIN(SmokeTest)
#include "tst_smoke.moc"
