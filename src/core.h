#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QFileDevice>
#include <QStringList>
#include <QVector>
#include <functional>

namespace conflictbench {

// Local regular files only. Callers must pause Syncthing and other writers.
// Checks detect observed changes; they cannot exclude a non-cooperating writer.
struct Options {
    int maxFiles = 100000;
    int maxConflicts = 1000;
    qint64 maxFileBytes = 256LL * 1024 * 1024;
    std::function<bool()> cancelled;
    std::function<void(int, const QString &)> progress;
    // Test-only injection seam. Empty in production. Called at named boundaries;
    // return an error to interrupt, or change synthetic fixtures to test races.
    std::function<QString(const QString &)> faultHook;
};

struct Snapshot {
    QString path;
    qint64 size = 0;
    QByteArray sha256;
    QFileDevice::Permissions permissions{};
    QDateTime modifiedUtc;
    // Windows owner/group/DACL, or Unix uid:gid, preserved on replacement.
    // Unix ACLs and extended attributes are conservatively unsupported.
    QByteArray nativeSecurity;
};
struct ConflictPair {
    QString root;
    Snapshot original;
    Snapshot conflict;
};
struct ScanResult {
    QString root;
    QVector<ConflictPair> pairs;
    QStringList warnings;
    QString error;
    bool cancelled = false;
};
struct Preview {
    QByteArray bytes;
    bool truncated = false;
    QString error;
};
enum class Action { KeepOriginal, UseConflict, KeepBoth };
struct Plan {
    ConflictPair pair;
    Action action = Action::KeepOriginal;
    QString backupRoot;
    QString keepBothPath;
    QString description;
    QString error;
};
struct Result {
    bool ok = false;
    QString error;
    QString receiptPath;
    QString state;
};
struct Receipt {
    QString path;
    QString root;
    QString originalPath;
    QString conflictPath;
    QString keepBothPath;
    QString state;
    QString action;
    QString createdUtc;
    QString error;
};

QString originalNameForConflict(const QString &fileName);
QString actionName(Action action);
ScanResult scan(const QString &root, const Options &options = {});
Preview preview(const Snapshot &snapshot, qint64 maxBytes = 1024 * 1024,
                const Options &options = {});
Plan plan(const ConflictPair &pair, Action action, const QString &backupRoot);
Result execute(const Plan &plan, bool pausedAcknowledged, const Options &options = {});
QVector<Receipt> history(const QString &backupRoot);
// Also rolls back an interrupted receipt. Refuses unrecognized current bytes,
// altered backups, symbolic links, missing required originals and new collisions.
// This is local rollback only; it cannot undo changes already synced elsewhere.
Result undo(const QString &receiptPath, bool pausedAcknowledged,
            const Options &options = {});

} // namespace conflictbench
