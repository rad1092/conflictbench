#include "core.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include <cstdlib>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#endif
#ifndef Q_OS_WIN
#include <unistd.h>
#include <sys/xattr.h>
#ifdef Q_OS_MACOS
#include <sys/acl.h>
#endif
#endif

using namespace conflictbench;
namespace {
#ifdef Q_OS_WIN
QString currentSid() {
    HANDLE token = nullptr; if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD size = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &size); QByteArray data(size, '\0');
    const bool ok = GetTokenInformation(token, TokenUser, data.data(), size, &size); CloseHandle(token); if (!ok) return {};
    LPWSTR sid = nullptr; if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid, &sid)) return {};
    const auto result = QString::fromWCharArray(sid); LocalFree(sid); return result;
}
bool setDacl(const QString &path, const QString &text) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<LPCWSTR>(text.utf16()), SDDL_REVISION_1, &descriptor, nullptr)) return false;
    const bool result = SetFileSecurityW(reinterpret_cast<LPCWSTR>(path.utf16()), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
    LocalFree(descriptor); return result;
}
QString securityText(const QString &path) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    constexpr SECURITY_INFORMATION fields = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    if (GetNamedSecurityInfoW(const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())), SE_FILE_OBJECT, fields, nullptr, nullptr, nullptr, nullptr, &descriptor) != ERROR_SUCCESS) return {};
    LPWSTR text = nullptr;
    const bool ok = ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor, SDDL_REVISION_1, fields, &text, nullptr);
    LocalFree(descriptor); if (!ok) return {};
    const auto result = QString::fromWCharArray(text); LocalFree(text); return result;
}
#endif
bool put(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}
QByteArray get(const QString &path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
struct Fixture {
    QTemporaryDir temporary;
    QString base;
    QString root;
    QString backups;
    QString original;
    QString conflict;
    Fixture(const QString &name = QStringLiteral("report.final.txt"), const QByteArray &a = "old version\n", const QByteArray &b = "new version\n") {
        base = QFileInfo(temporary.path()).canonicalFilePath();
        root = QDir(base).filePath("sync");
        backups = QDir(base).filePath("backups");
        QDir().mkdir(root); QDir().mkdir(backups); QDir().mkdir(QDir(root).filePath(".stfolder"));
        original = QDir(root).filePath(name);
        const int dot = name.lastIndexOf('.');
        const auto stem = dot < 0 ? name : name.left(dot);
        const auto ext = dot < 0 ? QString() : name.mid(dot);
        conflict = QDir(root).filePath(stem + ".sync-conflict-20261008-120000-ABCDEFG" + ext);
        put(original, a); put(conflict, b);
    }
    ConflictPair pair() const { const auto s = scan(root); return s.pairs.isEmpty() ? ConflictPair{} : s.pairs.front(); }
    Plan planned(Action a = Action::UseConflict) const { return plan(pair(), a, backups); }
};
}

class CoreTests : public QObject {
    Q_OBJECT
private slots:
    void parser_data() {
        QTest::addColumn<QString>("input"); QTest::addColumn<QString>("expected");
        QTest::newRow("dots") << "report.v2.sync-conflict-20261008-120000-ABCDEFG.txt" << "report.v2.txt";
        QTest::newRow("unicode") << QStringLiteral("한국어.문서.sync-conflict-20261008-120000-ABC2345.md") << QStringLiteral("한국어.문서.md");
        QTest::newRow("extensionless") << "README.sync-conflict-20261008-120000-ABCDEFG" << "README";
        QTest::newRow("dotfile") << ".sync-conflict-20261008-120000-ABCDEFG.env" << ".env";
        QTest::newRow("case_marker") << "file.Sync-Conflict-20261008-120000-ABCDEFG.txt" << "";
        QTest::newRow("case_device") << "file.sync-conflict-20261008-120000-abcdefg.txt" << "";
        QTest::newRow("invalid_date") << "file.sync-conflict-20260230-120000-ABCDEFG.txt" << "";
        QTest::newRow("invalid_time") << "file.sync-conflict-20261008-250000-ABCDEFG.txt" << "";
        QTest::newRow("ordinary") << "sync-conflict-not-a-real-conflict.txt" << "";
        QTest::newRow("compound-extension-original") << "report.txt.sync-conflict-20261008-120000-ABCDEFG.bak" << "report.txt.bak";
        QTest::newRow("not-last-extension") << "report.sync-conflict-20261008-120000-ABCDEFG.txt.bak" << "";
        QTest::newRow("windows-reserved") << "CON.sync-conflict-20261008-120000-ABCDEFG.txt" << "";
        QTest::newRow("windows-stream") << "file:stream.sync-conflict-20261008-120000-ABCDEFG.txt" << "";
        QTest::newRow("control") << "file\nname.sync-conflict-20261008-120000-ABCDEFG.txt" << "";
        QTest::newRow("syncthing-control") << ".sync-conflict-20261008-120000-ABCDEFG.stignore" << "";
    }
    void parser() { QFETCH(QString, input); QFETCH(QString, expected); QCOMPARE(originalNameForConflict(input), expected); }
    void unicodePairAndBoundedPreview() {
        Fixture f(QStringLiteral("한글.여러.점.txt"));
        const auto s = scan(f.root); QVERIFY2(s.error.isEmpty(), qPrintable(s.error)); QVERIFY2(s.pairs.size() == 1, qPrintable(s.warnings.join('\n')));
        const auto p = preview(s.pairs.front().conflict, 3); QVERIFY(p.error.isEmpty()); QCOMPARE(p.bytes, QByteArray("new")); QVERIFY(p.truncated);
    }
    void missingOriginalAndExactCase() {
        Fixture f;
        QVERIFY(QFile::rename(f.original, QDir(f.root).filePath("REPORT.FINAL.txt")));
        const auto s = scan(f.root); QVERIFY(s.pairs.isEmpty()); QVERIFY(!s.warnings.isEmpty());
    }
    void scanningDoesNotChangeSource() {
        Fixture f; const auto original = get(f.original); const auto conflict = get(f.conflict);
        const auto before = QFileInfo(f.original).lastModified(); QVERIFY(scan(f.root).error.isEmpty());
        QCOMPARE(get(f.original), original); QCOMPARE(get(f.conflict), conflict); QCOMPARE(QFileInfo(f.original).lastModified(), before);
    }
    void scanLimitsAndCancellation() {
        Fixture f; Options o; o.maxFiles = 1; QVERIFY(!scan(f.root, o).error.isEmpty());
        o = {}; o.maxFileBytes = 2; const auto s = scan(f.root, o); QVERIFY(s.pairs.isEmpty()); QVERIFY(!s.warnings.isEmpty());
        o = {}; o.cancelled = [] { return true; }; QVERIFY(scan(f.root, o).cancelled);
    }
    void refusesHomeAndDriveRoot() { QVERIFY(!scan(QDir::homePath()).error.isEmpty()); QVERIFY(!scan(QDir::rootPath()).error.isEmpty()); }
    void sameSizeMutationWithRestoredTimestamp() {
        Fixture f; const auto pair = f.pair(); const auto stamp = QFileInfo(f.original).lastModified();
        QVERIFY(put(f.original, "bad version\n"));
        QFile file(f.original); QVERIFY(file.open(QIODevice::ReadWrite)); QVERIFY(file.setFileTime(stamp, QFileDevice::FileModificationTime)); file.close();
        QVERIFY(!preview(pair.original).error.isEmpty());
        const auto result = execute(plan(pair, Action::UseConflict, f.backups), true);
        QVERIFY(!result.ok); QCOMPARE(result.state, QString("not_started")); QCOMPARE(get(f.original), QByteArray("bad version\n"));
    }
    void backupMustBeOutsideAllKnownSyncTrees() {
        Fixture f; const auto pair = f.pair();
        QVERIFY(!plan(pair, Action::UseConflict, f.root).error.isEmpty());
        const QString nested = QDir(f.root).filePath("sub"); QDir().mkdir(nested);
        QVERIFY(!plan(pair, Action::UseConflict, nested).error.isEmpty());
        QDir().mkdir(QDir(f.backups).filePath(".stfolder")); QVERIFY(!plan(pair, Action::UseConflict, f.backups).error.isEmpty());
    }
    void backupRejectsAncestorSyncRootEvenWhenScanningSubfolder() {
        Fixture f; const QString sub = QDir(f.root).filePath("sub"); const QString sibling = QDir(f.root).filePath("backups");
        QDir().mkdir(sub); QDir().mkdir(sibling);
        const QString a = QDir(sub).filePath(QFileInfo(f.original).fileName()); const QString b = QDir(sub).filePath(QFileInfo(f.conflict).fileName());
        QVERIFY(QFile::rename(f.original, a)); QVERIFY(QFile::rename(f.conflict, b));
        const auto s = scan(sub); QCOMPARE(s.pairs.size(), 1); QVERIFY(!plan(s.pairs.front(), Action::KeepOriginal, sibling).error.isEmpty());
    }
    void pauseRequired() { Fixture f; const auto r = execute(f.planned(), false); QVERIFY(!r.ok); QVERIFY(r.receiptPath.isEmpty()); QCOMPARE(get(f.original), QByteArray("old version\n")); }
    void actionsAndUndo_data() {
        QTest::addColumn<int>("action"); QTest::newRow("original") << 0; QTest::newRow("conflict") << 1; QTest::newRow("both") << 2;
    }
    void actionsAndUndo() {
        QFETCH(int, action); Fixture f; const auto p = f.planned(static_cast<Action>(action)); QVERIFY2(p.error.isEmpty(), qPrintable(p.error));
        const auto r = execute(p, true); QVERIFY2(r.ok, qPrintable(r.error)); QCOMPARE(r.state, QString("committed")); QVERIFY(!QFileInfo::exists(f.conflict));
        QCOMPARE(get(f.original), action == 1 ? QByteArray("new version\n") : QByteArray("old version\n"));
        if (action == 2) { QVERIFY(p.keepBothPath.endsWith(".txt")); QCOMPARE(get(p.keepBothPath), QByteArray("new version\n")); }
        const auto receipts = history(f.backups); QCOMPARE(receipts.size(), 1); QCOMPARE(receipts.front().state, QString("committed"));
        const auto u = undo(r.receiptPath, true); QVERIFY2(u.ok, qPrintable(u.error)); QCOMPARE(get(f.original), QByteArray("old version\n")); QCOMPARE(get(f.conflict), QByteArray("new version\n"));
        if (action == 2) QVERIFY(!QFileInfo::exists(p.keepBothPath));
        QVERIFY(!undo(r.receiptPath, true).ok); QCOMPARE(history(f.backups).front().state, QString("undone"));
    }
    void binaryExplicitChoice() {
        Fixture f("photo.bin", QByteArray::fromHex("000102ff"), QByteArray::fromHex("ff000204"));
        const auto p = f.planned(Action::KeepBoth); const auto r = execute(p, true); QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(get(f.original), QByteArray::fromHex("000102ff")); QCOMPARE(get(p.keepBothPath), QByteArray::fromHex("ff000204"));
    }
    void keepBothCollisionRefused() {
        Fixture f; const auto p = f.planned(Action::KeepBoth); QVERIFY(put(p.keepBothPath, "existing user file"));
        const auto r = execute(p, true); QVERIFY(!r.ok); QCOMPARE(get(p.keepBothPath), QByteArray("existing user file")); QVERIFY(QFileInfo::exists(f.conflict));
    }
    void laterChangeRefusesUndo() {
        Fixture f; const auto r = execute(f.planned(), true); QVERIFY(r.ok); QVERIFY(put(f.original, "later edit"));
        QVERIFY(!undo(r.receiptPath, true).ok); QCOMPARE(get(f.original), QByteArray("later edit")); QVERIFY(!QFileInfo::exists(f.conflict));
    }
    void corruptBackupRefusesUndo() {
        Fixture f; const auto r = execute(f.planned(), true); QVERIFY(r.ok);
        QVERIFY(put(QDir(QFileInfo(r.receiptPath).absolutePath()).filePath("original.backup"), "corrupt"));
        QVERIFY(!undo(r.receiptPath, true).ok); QCOMPARE(get(f.original), QByteArray("new version\n")); QVERIFY(!QFileInfo::exists(f.conflict));
    }
    void sourceMutationAtCommitBoundaryAborts() {
        Fixture f; const auto p = f.planned(); Options o;
        o.faultHook = [&](const QString &stage) { if (stage == "destination_commit") put(f.original, "changed after review"); return QString(); };
        const auto r = execute(p, true, o); QVERIFY(!r.ok); QCOMPARE(get(f.original), QByteArray("changed after review")); QCOMPARE(get(f.conflict), QByteArray("new version\n"));
        QVERIFY(!undo(r.receiptPath, true).ok);
    }
    void mutationAfterBackupsAborts() {
        Fixture f; const auto p = f.planned(); Options o;
        o.faultHook = [&](const QString &stage) { if (stage == "after_backups") put(f.conflict, "bad version\n"); return QString(); };
        const auto r = execute(p, true, o); QVERIFY(!r.ok); QCOMPARE(get(f.original), QByteArray("old version\n")); QCOMPARE(get(f.conflict), QByteArray("bad version\n"));
    }
    void preparedStageRejectsLaterRecognizedBytesAndNewKeptCopy() {
        Fixture f; const auto p = f.planned(); Options o;
        o.faultHook = [](const QString &s) { return s == "before_destination" ? QString("stop before write") : QString(); };
        const auto r = execute(p, true, o); QVERIFY(!r.ok); QVERIFY(put(f.original, "new version\n"));
        QVERIFY(!undo(r.receiptPath, true).ok); QCOMPARE(get(f.original), QByteArray("new version\n"));
        Fixture g; const auto bothPlan = g.planned(Action::KeepBoth); const auto bothResult = execute(bothPlan, true, o); QVERIFY(!bothResult.ok);
        QVERIFY(put(bothPlan.keepBothPath, "new version\n")); QVERIFY(!undo(bothResult.receiptPath, true).ok); QVERIFY(QFileInfo::exists(bothPlan.keepBothPath));
    }
    void unrecordedDestinationRefusesLaterMatchingWrite_data() {
        QTest::addColumn<int>("action");
        QTest::newRow("replace-original") << 1;
        QTest::newRow("create-kept-copy") << 2;
    }
    void unrecordedDestinationRefusesLaterMatchingWrite() {
        QFETCH(int, action); Fixture f; const auto p = f.planned(static_cast<Action>(action));
        int copies = 0; Options o;
        o.faultHook = [&](const QString &stage) {
            return stage == "before_copy_commit" && ++copies == 3 ? QString("Stop before destination rename") : QString();
        };
        const auto interrupted = execute(p, true, o); QVERIFY(!interrupted.ok);
        QCOMPARE(history(f.backups).front().state, QString("writing_destination"));
        const QString target = action == 1 ? f.original : p.keepBothPath;
        QVERIFY(put(target, get(f.conflict)));
        QFile file(target); QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(QDateTime::currentDateTimeUtc().addSecs(10), QFileDevice::FileModificationTime)); file.close();
        const auto recovered = undo(interrupted.receiptPath, true);
        QVERIFY(!recovered.ok); QVERIFY(recovered.error.contains("durable post-state"));
        QCOMPARE(get(target), QByteArray("new version\n"));
        QCOMPARE(get(f.conflict), QByteArray("new version\n"));
    }
    void receiptPathTraversalRefused() {
        Fixture f; const auto r = execute(f.planned(), true); QVERIFY(r.ok);
        auto doc = QJsonDocument::fromJson(get(r.receiptPath)).object();
        auto original = doc["original"].toObject(); original["path"] = f.root + "/../outside/report.final.txt"; doc["original"] = original;
        QVERIFY(put(r.receiptPath, QJsonDocument(doc).toJson())); QVERIFY(!undo(r.receiptPath, true).ok); QCOMPARE(get(f.original), QByteArray("new version\n"));
    }
    void extendedDataStreamsRefusedBeforeBackup() {
        Fixture f;
#ifdef Q_OS_WIN
        QVERIFY(put(f.conflict + ":private_stream", "alternate data"));
#elif defined(Q_OS_MACOS)
        const QByteArray name = QFile::encodeName(f.conflict);
        QVERIFY(::setxattr(name.constData(), "com.apple.ResourceFork", "FORK", 4, 0, 0) == 0);
#else
        const QByteArray name = QFile::encodeName(f.conflict);
        QVERIFY(::setxattr(name.constData(), "user.conflictbench-test", "ATTR", 4, 0) == 0);
#endif
        const auto s = scan(f.root); QVERIFY(s.pairs.isEmpty()); QVERIFY(!s.warnings.isEmpty()); QCOMPARE(get(f.original), QByteArray("old version\n")); QVERIFY(QFileInfo::exists(f.conflict));
        QVERIFY(QDir(f.backups).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    }
    void metadataAddedAfterScanRefused() {
        Fixture f; const auto p = f.planned();
#ifdef Q_OS_WIN
        QVERIFY(put(f.conflict + ":late_stream", "alternate data"));
#elif defined(Q_OS_MACOS)
        QVERIFY(::setxattr(QFile::encodeName(f.conflict).constData(), "com.apple.ResourceFork", "FORK", 4, 0, 0) == 0);
#else
        QVERIFY(::setxattr(QFile::encodeName(f.conflict).constData(), "user.conflictbench-test", "ATTR", 4, 0) == 0);
#endif
        const auto r = execute(p, true); QVERIFY(!r.ok); QVERIFY(r.receiptPath.isEmpty()); QCOMPARE(get(f.original), QByteArray("old version\n")); QVERIFY(QFileInfo::exists(f.conflict));
    }
    void inheritableBackupAclRefused() {
#ifdef Q_OS_MACOS
        Fixture f; const auto pair = f.pair();
        const int rc = QProcess::execute("/bin/chmod", {"+a", "everyone allow read,execute,readattr,readextattr,readsecurity,file_inherit,directory_inherit", f.backups});
        QCOMPARE(rc, 0); const auto p = plan(pair, Action::UseConflict, f.backups); QVERIFY(!p.error.isEmpty());
        QVERIFY(QDir(f.backups).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()); QProcess::execute("/bin/chmod", {"-N", f.backups});
#else
        QSKIP("Darwin inherited ACL regression; Windows private DACL is created explicitly.");
#endif
    }
    void interruptedTransactionsRecover_data() {
        QTest::addColumn<QString>("stage"); QTest::addColumn<int>("action");
        QTest::newRow("backup-write") << "copy_chunk" << 1;
        QTest::newRow("after-backups") << "after_backups" << 1;
        QTest::newRow("before-destination") << "before_destination" << 1;
        QTest::newRow("after-destination") << "after_destination" << 1;
        QTest::newRow("after-both") << "after_destination" << 2;
        QTest::newRow("after-remove") << "after_remove_conflict" << 1;
        QTest::newRow("after-remove-original") << "after_remove_conflict" << 0;
        QTest::newRow("after-remove-both") << "after_remove_conflict" << 2;
    }
    void interruptedTransactionsRecover() {
        QFETCH(QString, stage); QFETCH(int, action); Fixture f; const auto p = f.planned(static_cast<Action>(action)); Options o;
        o.faultHook = [&](const QString &s) { return s == stage ? QString("Injected disk-full/interruption at %1").arg(s) : QString(); };
        const auto r = execute(p, true, o); QVERIFY(!r.ok); QVERIFY(!r.receiptPath.isEmpty());
        const auto u = undo(r.receiptPath, true); QVERIFY2(u.ok, qPrintable(u.error)); QCOMPARE(get(f.original), QByteArray("old version\n")); QCOMPARE(get(f.conflict), QByteArray("new version\n"));
        if (action == 2) QVERIFY(!QFileInfo::exists(p.keepBothPath));
    }
    void cancellationAfterDestinationRecoverable() {
        Fixture f; const auto p = f.planned(); bool cancel = false; Options o;
        o.cancelled = [&] { return cancel; }; o.faultHook = [&](const QString &s) { if (s == "after_destination") cancel = true; return QString(); };
        const auto r = execute(p, true, o); QVERIFY(!r.ok); const auto u = undo(r.receiptPath, true); QVERIFY2(u.ok, qPrintable(u.error)); QCOMPARE(get(f.original), QByteArray("old version\n"));
    }
    void concurrentTransactionBlockedAcrossBackupDirectories() {
        Fixture f; const auto p = f.planned(); const QString second = QDir(f.base).filePath("backup2"); QDir().mkdir(second); const auto p2 = plan(p.pair, Action::KeepBoth, second);
        bool attempted = false; Result concurrent; Options o;
        o.faultHook = [&](const QString &s) { if (s == "after_backups") { attempted = true; concurrent = execute(p2, true); } return QString(); };
        const auto r = execute(p, true, o); QVERIFY2(r.ok, qPrintable(r.error)); QVERIFY(attempted); QVERIFY(!concurrent.ok); QVERIFY(concurrent.error.contains("lock") || concurrent.error.contains("active"));
        QVERIFY(QDir(second).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    }
    void crashRestartRecovery_data() {
        QTest::addColumn<QString>("stage");
        QTest::newRow("crash-after-backups") << "after_backups";
        QTest::newRow("crash-after-write") << "after_destination";
        QTest::newRow("crash-after-remove") << "after_remove_conflict";
    }
    void crashRestartRecovery() {
        QFETCH(QString, stage); Fixture f; QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--crash-stage", stage, f.root, f.backups});
        QVERIFY(child.waitForStarted()); QVERIFY(child.waitForFinished(15000)); QCOMPARE(child.exitCode(), 42);
        const auto list = history(f.backups); QCOMPARE(list.size(), 1); QVERIFY(list.front().state != "committed");
        const auto r = undo(list.front().path, true); QVERIFY2(r.ok, qPrintable(r.error)); QCOMPARE(get(f.original), QByteArray("old version\n")); QCOMPARE(get(f.conflict), QByteArray("new version\n"));
    }
    void interruptedUndoCanResume_data() {
        QTest::addColumn<QString>("stage"); QTest::addColumn<int>("action");
        QTest::newRow("restore-conflict") << "after_undo_conflict" << 1;
        QTest::newRow("restore-original") << "after_undo_original" << 1;
        QTest::newRow("remove-both") << "undo_remove_both" << 2;
        QTest::newRow("finish-both") << "before_undo_complete" << 2;
    }
    void interruptedUndoCanResume() {
        QFETCH(QString, stage); QFETCH(int, action); Fixture f; const auto r = execute(f.planned(static_cast<Action>(action)), true); QVERIFY(r.ok); Options o;
        o.faultHook = [&](const QString &s) { return s == stage ? QString("Injected undo interruption") : QString(); };
        QVERIFY(!undo(r.receiptPath, true, o).ok); const auto u = undo(r.receiptPath, true); QVERIFY2(u.ok, qPrintable(u.error)); QCOMPARE(get(f.original), QByteArray("old version\n")); QCOMPARE(get(f.conflict), QByteArray("new version\n"));
    }
    void malformedReceiptRefused() { Fixture f; const QString file = QDir(f.backups).filePath("receipt.json"); QVERIFY(put(file, "{bad json")); QVERIFY(!undo(file, true).ok); }
    void symlinkFilesAndAncestorsRefused() {
        Fixture f;
#ifdef Q_OS_WIN
        QSKIP("Windows junction/reparse coverage is exercised by the Windows-specific test below.");
#else
        const QString external = QDir(f.base).filePath("outside.txt"); QVERIFY(put(external, "outside"));
        QVERIFY(QFile::remove(f.original)); QVERIFY(QFile::link(external, f.original)); const auto s = scan(f.root); QVERIFY(s.pairs.isEmpty()); QVERIFY(!s.warnings.isEmpty());
        const QString alias = QDir(f.base).filePath("alias"); QVERIFY(QFile::link(f.root, alias)); QVERIFY(!scan(alias).error.isEmpty());
        Fixture cleanFixture; const auto p = cleanFixture.planned(); const QString aliasBackup = QDir(cleanFixture.base).filePath("backup-alias"); QVERIFY(QFile::link(cleanFixture.backups, aliasBackup));
        QVERIFY(!plan(p.pair, Action::KeepOriginal, aliasBackup).error.isEmpty()); QCOMPARE(get(external), QByteArray("outside"));
#endif
    }
    void permissionDeniedLeavesSourceUnchanged() {
#ifdef Q_OS_WIN
        QSKIP("Unix directory permission test; Windows ACL creation is covered by Windows transaction tests.");
#else
        if (::geteuid() == 0) QSKIP("Permission denial cannot be asserted when running as root.");
        Fixture f; const auto p = f.planned(); const auto oldMode = QFileInfo(f.backups).permissions();
        QVERIFY(QFile::setPermissions(f.backups, QFileDevice::ReadOwner | QFileDevice::ExeOwner)); const auto r = execute(p, true); QFile::setPermissions(f.backups, oldMode);
        QVERIFY(!r.ok); QCOMPARE(get(f.original), QByteArray("old version\n")); QCOMPARE(get(f.conflict), QByteArray("new version\n"));
#endif
    }
    void privateBackupPermissions() {
        Fixture f; const auto r = execute(f.planned(), true); QVERIFY(r.ok);
#ifndef Q_OS_WIN
        const auto directory = QFileInfo(r.receiptPath).absolutePath();
        const auto forbidden = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
        QVERIFY(!(QFileInfo(directory).permissions() & forbidden)); QVERIFY(!(QFileInfo(QDir(directory).filePath("original.backup")).permissions() & forbidden));
#endif
    }
    void hardLinksRefused() {
        Fixture f; const QString other = QDir(f.base).filePath("hardlinked.txt");
#ifdef Q_OS_WIN
        QVERIFY(CreateHardLinkW(reinterpret_cast<LPCWSTR>(other.utf16()), reinterpret_cast<LPCWSTR>(f.original.utf16()), nullptr));
#else
        QVERIFY(::link(QFile::encodeName(f.original).constData(), QFile::encodeName(other).constData()) == 0);
#endif
        const auto s = scan(f.root); QVERIFY(s.pairs.isEmpty()); QVERIFY(!s.warnings.isEmpty()); QCOMPARE(get(other), QByteArray("old version\n"));
    }
    void windowsJunctionAndNativePermissions() {
#ifdef Q_OS_WIN
        Fixture f;
        const QString junction = QDir(f.base).filePath("junction");
        QCOMPARE(QProcess::execute("cmd.exe", {"/C", "mklink", "/J", QDir::toNativeSeparators(junction), QDir::toNativeSeparators(f.root)}), 0);
        QVERIFY(!scan(junction).error.isEmpty()); QVERIFY(QDir().rmdir(junction));
        const QString sid = currentSid(); QVERIFY(!sid.isEmpty());
        QVERIFY(setDacl(f.original, "D:P(A;;FA;;;" + sid + ")"));
        QVERIFY(setDacl(f.backups, "D:P(A;OICI;FA;;;" + sid + ")(A;OICI;GR;;;WD)"));
        const auto originalAcl = securityText(f.original); QVERIFY(!originalAcl.isEmpty());
        const auto conflictAcl = securityText(f.conflict); QVERIFY(!conflictAcl.isEmpty());
        const auto r = execute(f.planned(), true); QVERIFY2(r.ok, qPrintable(r.error)); QCOMPARE(securityText(f.original), originalAcl);
        const QString privateBackup = QDir(QFileInfo(r.receiptPath).absolutePath()).filePath("original.backup");
        const auto backupAcl = securityText(privateBackup); QVERIFY(!backupAcl.isEmpty()); QVERIFY(!backupAcl.contains(";;;WD)"));
        const auto u = undo(r.receiptPath, true); QVERIFY2(u.ok, qPrintable(u.error)); QCOMPARE(securityText(f.original), originalAcl); QCOMPARE(securityText(f.conflict), conflictAcl);
#else
        QSKIP("Windows native junction/DACL test runs in Windows CI.");
#endif
    }
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("ConflictBench-core-tests");
    QStandardPaths::setTestModeEnabled(true);
    const auto args = app.arguments();
    if (args.size() == 5 && args[1] == "--crash-stage") {
        const auto s = scan(args[3]); if (s.pairs.size() != 1) return 2;
        Options o; o.faultHook = [&](const QString &stage) { if (stage == args[2]) std::_Exit(42); return QString(); };
        execute(plan(s.pairs.front(), Action::UseConflict, args[4]), true, o); return 3;
    }
    CoreTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "core_tests.moc"
