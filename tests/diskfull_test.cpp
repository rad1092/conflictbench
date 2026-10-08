#include "core.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#ifndef Q_OS_LINUX
#error "The real disk-full test requires a dedicated Linux tmpfs."
#endif
#include <fcntl.h>
#include <linux/magic.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>

using namespace conflictbench;

namespace {
constexpr qint64 KiB = 1024;
constexpr qint64 maximumVolumeBytes = 4 * 1024 * KiB;

void require(bool condition, const QString &message) {
    if (!condition) throw std::runtime_error(message.toStdString());
}

void event(const QJsonObject &details) {
    QTextStream(stdout) << QJsonDocument(details).toJson(QJsonDocument::Compact) << '\n';
}

qint64 availableBytes(const QString &path) {
    struct statvfs volume {};
    require(::statvfs(QFile::encodeName(path).constData(), &volume) == 0,
            "Cannot inspect tmpfs free space.");
    return qint64(volume.f_bavail) * qint64(volume.f_frsize);
}

QString validateEmptyMount(const QString &input) {
    require(QDir::isAbsolutePath(input), "An absolute dedicated mount path is required.");
    const QString path = QDir::cleanPath(input);
    const QFileInfo info(path);
    require(info.isDir() && !info.isSymLink() && info.canonicalFilePath() == path,
            "The mount must be a canonical directory without symlink aliases.");
    struct stat current {}, parent {};
    require(::lstat(QFile::encodeName(path).constData(), &current) == 0 &&
            ::lstat(QFile::encodeName(info.absolutePath()).constData(), &parent) == 0,
            "Cannot inspect dedicated mount ownership.");
    require(current.st_dev != parent.st_dev, "The supplied path is not a separate mount root.");
    require(current.st_uid == ::geteuid() && (current.st_mode & 0777) == 0700,
            "The dedicated mount must belong to the test user with mode 0700.");
    struct statfs nativeVolume {};
    require(::statfs(QFile::encodeName(path).constData(), &nativeVolume) == 0 &&
            nativeVolume.f_type == TMPFS_MAGIC, "Refusing to fill a non-tmpfs filesystem.");
    const QStorageInfo volume(path);
    require(volume.isValid() && volume.isReady() && volume.fileSystemType() == "tmpfs" &&
            QFileInfo(volume.rootPath()).canonicalFilePath() == path,
            "Qt must also identify this exact directory as a tmpfs mount root.");
    require(volume.bytesTotal() >= 1024 * KiB && volume.bytesTotal() <= maximumVolumeBytes,
            "Refusing to fill a tmpfs outside the 1–4 MiB test-size range.");
    require(QDir(path).entryList(QDir::AllEntries | QDir::Hidden | QDir::System |
                               QDir::NoDotAndDotDot).isEmpty(),
            "Refusing a mount that already contains files.");
    event({{"event", "validated_mount"}, {"filesystem", "tmpfs"},
           {"bytes_total", double(volume.bytesTotal())}, {"uid", int(::geteuid())}});
    return path;
}

void put(const QString &path, const QByteArray &data) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "Cannot create synthetic fixture: " + path);
    require(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner), "Cannot restrict fixture permissions.");
    const qint64 written = file.write(data);
    require(written == data.size() && file.flush(), "Cannot write complete synthetic fixture.");
}

QByteArray read(const QString &path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot read test evidence: " + path);
    const auto bytes = file.readAll();
    require(file.error() == QFileDevice::NoError, "Cannot read complete test evidence.");
    return bytes;
}

void mkdir(const QString &path) {
    require(QDir().mkdir(path), "Cannot create test directory: " + path);
    require(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                      QFileDevice::ExeOwner), "Cannot restrict test directory permissions.");
}

struct FileWitness {
    QString path;
    QByteArray bytes;
    struct stat original {};

    explicit FileWitness(const QString &filePath) : path(filePath), bytes(read(filePath)) {
        require(::lstat(QFile::encodeName(path).constData(), &original) == 0,
                "Cannot record source file metadata.");
    }

    void assertUnchanged() const {
        struct stat now {};
        require(::lstat(QFile::encodeName(path).constData(), &now) == 0,
                "Source disappeared during disk-full failure: " + path);
        require(read(path) == bytes && now.st_dev == original.st_dev &&
                now.st_ino == original.st_ino && now.st_size == original.st_size &&
                now.st_mode == original.st_mode && now.st_uid == original.st_uid &&
                now.st_gid == original.st_gid && now.st_nlink == original.st_nlink &&
                now.st_mtim.tv_sec == original.st_mtim.tv_sec &&
                now.st_mtim.tv_nsec == original.st_mtim.tv_nsec,
                "Source bytes, identity, modification time, or permissions changed: " + path);
    }
};

// Real allocations only: no sparse file, configured fault return, or mocked I/O.
// This fd owns just one new file on the size-checked dedicated mount.
class CapacityFiller {
    QString path;
    int fd = -1;
    bool ownsPath = false;
    qint64 total = 0;
public:
    CapacityFiller(const QString &mount, const QString &scenario, qint64 reserveBytes)
        : path(QDir(mount).filePath("owned-capacity-filler")) {
        fd = ::open(QFile::encodeName(path).constData(), O_WRONLY | O_CREAT | O_EXCL |
                    O_CLOEXEC | O_NOFOLLOW, 0600);
        require(fd >= 0, "Cannot create exclusive capacity filler.");
        ownsPath = true;
        try {
            const QByteArray block(64 * KiB, 'F');
            int fullError = 0;
            while (total <= maximumVolumeBytes + block.size()) {
                errno = 0;
                const auto count = ::write(fd, block.constData(), size_t(block.size()));
                if (count > 0) { total += count; continue; }
                if (count < 0 && errno == EINTR) continue;
                fullError = count < 0 ? errno : 0;
                break;
            }
            require(fullError == ENOSPC, "The real filler write did not fail with Linux ENOSPC.");
            require(availableBytes(mount) == 0, "ENOSPC did not exhaust the dedicated volume's data capacity.");
            require(total > reserveBytes, "Insufficient bounded filler allocation.");
            require(::ftruncate(fd, off_t(total - reserveBytes)) == 0 && ::fsync(fd) == 0,
                    "Cannot reserve deterministic staging headroom.");
            event({{"event", "native_enospc"}, {"scenario", scenario}, {"errno", fullError},
                   {"errno_name", "ENOSPC"}, {"error", QString::fromLocal8Bit(std::strerror(fullError))},
                   {"filler_bytes_before_reserve", double(total)},
                   {"available_before_reserve", 0},
                   {"available_after_reserve", double(availableBytes(mount))}});
        } catch (...) {
            ::close(fd); fd = -1;
            QFile::remove(path);
            throw;
        }
    }

    CapacityFiller(const CapacityFiller &) = delete;
    CapacityFiller &operator=(const CapacityFiller &) = delete;
    ~CapacityFiller() { if (fd >= 0) ::close(fd); if (ownsPath) QFile::remove(path); }

    void release() {
        require(fd >= 0, "Capacity filler was already released.");
        const int handle = fd; fd = -1;
        require(::close(handle) == 0, "Cannot close capacity filler.");
        require(QFile::remove(path), "Cannot remove owned capacity filler.");
        ownsPath = false;
    }
};

QString receiptState(const QString &path) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(read(path), &error);
    require(error.error == QJsonParseError::NoError && document.isObject(), "Durable receipt is not readable JSON.");
    require(document.object().value("format") == "conflictbench-1", "Unexpected receipt format.");
    return document.object().value("state").toString();
}

void exercise(const QString &mount, const QString &external, bool fullBackupVolume) {
    const QString scenario = fullBackupVolume ? "backup_volume_full" : "destination_volume_full";
    const QString sourceRoot = QDir(fullBackupVolume ? external : mount).filePath(scenario + "-source");
    const QString backupRoot = QDir(fullBackupVolume ? mount : external).filePath(scenario + "-backups");
    mkdir(sourceRoot); mkdir(backupRoot); mkdir(QDir(sourceRoot).filePath(".stfolder"));
    const QString original = QDir(sourceRoot).filePath(QStringLiteral("한글.report.final.bin"));
    const QString conflict = QDir(sourceRoot).filePath(QStringLiteral("한글.report.final.sync-conflict-20261008-120000-ABCDEFG.bin"));
    const QByteArray originalBytes(64 * KiB, 'A');
    const QByteArray conflictBytes(512 * KiB, 'B');
    put(original, originalBytes); put(conflict, conflictBytes);
    const FileWitness originalBefore(original), conflictBefore(conflict);
    const auto scanned = scan(sourceRoot);
    require(scanned.error.isEmpty() && scanned.pairs.size() == 1,
            "Cannot scan synthetic pair: " + scanned.error + scanned.warnings.join('\n'));
    const auto proposed = plan(scanned.pairs.front(), Action::UseConflict, backupRoot);
    require(proposed.error.isEmpty(), "Cannot plan synthetic transaction: " + proposed.error);

    CapacityFiller filler(mount, scenario, fullBackupVolume ? 160 * KiB : 64 * KiB);
    const qint64 free = availableBytes(mount);
    require(free > originalBytes.size() / 2 && free < conflictBytes.size(),
            "The volume has unexpected free space before the transaction.");
    if (fullBackupVolume)
        require(free >= originalBytes.size() + 16 * KiB, "Cannot stage the first backup and receipt.");

    // No faultHook is supplied: this must reach the real QSaveFile write.
    const auto result = execute(proposed, true);
    require(!result.ok && result.state == "interrupted" && !result.receiptPath.isEmpty(),
            "Disk-full transaction did not report an interrupted, uncommitted result.");
    require(result.error.startsWith("Destination write failed (space or permissions):"),
            "Expected an actual destination data write failure; received: " + result.error);
    const QString expectedState = fullBackupVolume ? "backing_up" : "writing_destination";
    require(receiptState(result.receiptPath) == expectedState, "The last durable transaction stage was lost.");
    const auto entries = history(backupRoot);
    require(entries.size() == 1 && entries.front().state == expectedState && entries.front().error.isEmpty(),
            "History did not expose the exact interrupted transaction.");
    const QDir transaction(QFileInfo(result.receiptPath).absolutePath());
    require(read(transaction.filePath("original.backup")) == originalBytes,
            "The first staged backup is missing or incorrect.");
    if (fullBackupVolume)
        require(!QFileInfo::exists(transaction.filePath("conflict.backup")),
                "An incomplete second backup was incorrectly committed.");
    else
        require(read(transaction.filePath("conflict.backup")) == conflictBytes,
                "The verified conflict backup is missing or incorrect.");
    originalBefore.assertUnchanged(); conflictBefore.assertUnchanged();
    event({{"event", "transaction_interrupted"}, {"scenario", scenario},
           {"durable_state", expectedState}, {"error", result.error},
           {"source_bytes_and_metadata_unchanged", true}, {"committed", false}});

    // Only our own filler is removed. Recovery uses the returned receipt path,
    // never an inferred/latest transaction and never an automatic retry.
    filler.release();
    const auto restored = undo(result.receiptPath, true);
    require(restored.ok && restored.state == "undone", "Explicit local recovery failed: " + restored.error);
    require(receiptState(result.receiptPath) == "undone", "Recovery did not durably record completion.");
    originalBefore.assertUnchanged(); conflictBefore.assertUnchanged();
    const auto finalEntries = history(backupRoot);
    require(finalEntries.size() == 1 && finalEntries.front().state == "undone", "Recovered history is inconsistent.");
    event({{"event", "recovery_passed"}, {"scenario", scenario},
           {"source_bytes_and_metadata_unchanged", true}, {"durable_state", "undone"}});
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("ConflictBench-diskfull-test");
    if (app.arguments().size() != 2) {
        QTextStream(stderr) << "Usage: diskfull_test /absolute/path/to/empty-private-tmpfs\n";
        return 2;
    }
    try {
        const auto mount = validateEmptyMount(app.arguments().at(1));
        QTemporaryDir external(QDir(QFileInfo(mount).absolutePath()).filePath("owned-fixtures-XXXXXX"));
        require(external.isValid(), "Cannot create private synthetic fixture directory outside tmpfs.");
        const QString base = QFileInfo(external.path()).canonicalFilePath();
        struct stat outside {}, inside {};
        require(::stat(QFile::encodeName(base).constData(), &outside) == 0 &&
                ::stat(QFile::encodeName(mount).constData(), &inside) == 0 && outside.st_dev != inside.st_dev,
                "Synthetic recovery/source storage must be outside the full volume.");
        // Keep cbcore's transaction lock in the owned fixture tree, not user data.
        const QString appData = QDir(base).filePath("app-data");
        mkdir(appData);
        qputenv("XDG_DATA_HOME", QFile::encodeName(appData));
        exercise(mount, base, true);
        exercise(mount, base, false);
        event({{"event", "diskfull_suite_passed"}, {"scenarios", 2}});
        return 0;
    } catch (const std::exception &error) {
        QTextStream(stderr) << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
