#include "core.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUuid>
#include <memory>
#include <cerrno>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <sys/xattr.h>
#ifdef Q_OS_MACOS
#include <sys/acl.h>
#endif
#include <fcntl.h>
#include <unistd.h>
#endif

namespace conflictbench {
namespace {
const auto privateFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
struct Failure { QString message; };
void require(bool condition, const QString &message) { if (!condition) throw Failure{message}; }
void checkpoint(const Options &o, const QString &name) {
    if (o.cancelled && o.cancelled()) throw Failure{QStringLiteral("Cancelled. Inspect history before retrying any interrupted transaction.")};
    if (o.faultHook) { const QString error = o.faultHook(name); if (!error.isEmpty()) throw Failure{error}; }
}
QString clean(const QString &path) { return QDir::cleanPath(QFileInfo(path).absoluteFilePath()); }
void localFilesystem(const QString &path) {
    const QStorageInfo volume(path);
    static const QSet<QByteArray> supported{"apfs", "hfs", "hfsplus", "ext2", "ext3", "ext4", "xfs", "btrfs", "tmpfs", "zfs", "overlay", "ntfs", "refs"};
    require(volume.isValid() && volume.isReady() && supported.contains(volume.fileSystemType().toLower()), "Unsupported or nonlocal filesystem; use a regular local volume: " + path);
#ifdef Q_OS_WIN
    const QString volumeRoot = QDir::toNativeSeparators(volume.rootPath());
    const UINT drive = GetDriveTypeW(reinterpret_cast<LPCWSTR>(volumeRoot.utf16()));
    require(drive == DRIVE_FIXED || drive == DRIVE_REMOVABLE || drive == DRIVE_RAMDISK, "Network and unknown volumes are unsupported: " + path);
#endif
}
bool within(const QString &path, const QString &directory) {
#ifdef Q_OS_WIN
    constexpr auto cs = Qt::CaseInsensitive;
#else
    constexpr auto cs = Qt::CaseSensitive;
#endif
    return path.compare(directory, cs) == 0 || path.startsWith(directory + '/', cs);
}
// Every component is checked, including existing ancestors of a new file.
// Re-checking reduces accidental races; it does not create a sandbox against
// a hostile local process that can rename directories during this operation.
void safePath(const QString &path, bool allowMissingLeaf = false) {
    require(QDir::isAbsolutePath(path), "An absolute local path is required.");
    QString current = clean(path);
    bool leaf = true;
    while (true) {
        QFileInfo info(current);
#ifdef Q_OS_WIN
        const DWORD attrs = GetFileAttributesW(reinterpret_cast<LPCWSTR>(current.utf16()));
        const bool exists = attrs != INVALID_FILE_ATTRIBUTES;
        require(!exists || !(attrs & FILE_ATTRIBUTE_REPARSE_POINT), "Reparse points and junctions are not supported: " + current);
#else
        struct stat st{};
        const bool exists = ::lstat(QFile::encodeName(current).constData(), &st) == 0;
        require(!exists || !S_ISLNK(st.st_mode), "Symbolic links are not supported: " + current);
#endif
        require(exists || (leaf && allowMissingLeaf), "Path is missing or inaccessible: " + current);
        if (exists && !leaf) require(info.isDir(), "Path ancestor is not a directory: " + current);
        const QString parent = info.absolutePath();
        if (parent == current) break;
        current = parent;
        leaf = false;
    }
}
QString safeDirectory(const QString &path) {
    safePath(clean(path));
    const QFileInfo info(path);
    require(info.isDir(), "Select an existing local directory: " + path);
    const QString canonical = info.canonicalFilePath();
    require(!canonical.isEmpty(), "Cannot resolve directory: " + path);
    return QDir::cleanPath(canonical);
}
QString markerAncestor(const QString &path) {
    QString d = clean(path);
    while (true) {
        QFileInfo marker(QDir(d).filePath(".stfolder"));
        if (marker.exists() || marker.isSymLink()) return d;
        const QString parent = QFileInfo(d).absolutePath();
        if (parent == d) return {};
        d = parent;
    }
}
void noDirectoryAcl(const QString &path) {
#ifdef Q_OS_MACOS
    errno = 0;
    acl_t acl = acl_get_file(QFile::encodeName(path).constData(), ACL_TYPE_EXTENDED);
    // Darwin also reports ENOENT for an existing file with no extended ACL.
    if (!acl && errno == ENOENT && QFileInfo::exists(path)) return;
    require(acl != nullptr, "Cannot inspect directory access-control list: " + path);
    acl_entry_t entry;
    const int first = acl_get_entry(acl, ACL_FIRST_ENTRY, &entry);
    acl_free(acl);
    // Darwin returns 0 for an entry and -1 when the list is empty.
    require(first == -1, "Extended directory access-control lists are unsupported: " + path);
#elif !defined(Q_OS_WIN)
    const QByteArray name = QFile::encodeName(path);
    const ssize_t count = ::llistxattr(name.constData(), nullptr, 0);
    require(count >= 0, "Cannot inspect directory metadata: " + path);
    if (count > 0) {
        QByteArray names(count, '\0');
        require(::llistxattr(name.constData(), names.data(), names.size()) == count, "Directory metadata changed.");
        require(!names.contains("system.posix_acl_"), "Extended directory access-control lists are unsupported: " + path);
    }
#else
    Q_UNUSED(path); // privateMkdir supplies a protected per-user Windows DACL.
#endif
}
QByteArray nativeMetadata(const QString &path) {
#ifdef Q_OS_WIN
    const DWORD attrs = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
    constexpr DWORD unsupported = FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | 0x00040000 | 0x00400000;
    require(attrs != INVALID_FILE_ATTRIBUTES && !(attrs & unsupported), "File has unsupported Windows attributes (hidden/system, streams, encryption, sparse, cloud or compressed): " + path);
    const HANDLE identity = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    require(identity != INVALID_HANDLE_VALUE, "Cannot inspect Windows file identity.");
    BY_HANDLE_FILE_INFORMATION identityInfo{};
    const bool identityOk = GetFileInformationByHandle(identity, &identityInfo);
    CloseHandle(identity);
    require(identityOk && identityInfo.nNumberOfLinks == 1, "Hard-linked files are unsupported: " + path);
    WIN32_FIND_STREAM_DATA stream{};
    const HANDLE search = FindFirstStreamW(reinterpret_cast<LPCWSTR>(path.utf16()), FindStreamInfoStandard, &stream, 0);
    if (search == INVALID_HANDLE_VALUE) require(GetLastError() == ERROR_HANDLE_EOF, "Cannot inspect file streams; local NTFS/ReFS files are required: " + path);
    else {
        bool alternate = false;
        do { if (QString::fromWCharArray(stream.cStreamName) != "::$DATA") alternate = true; } while (FindNextStreamW(search, &stream));
        const DWORD streamError = GetLastError(); FindClose(search);
        require(!alternate && streamError == ERROR_HANDLE_EOF, "Alternate data streams are unsupported; no files changed: " + path);
    }
    constexpr SECURITY_INFORMATION fields = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    DWORD size = 0;
    GetFileSecurityW(reinterpret_cast<LPCWSTR>(path.utf16()), fields, nullptr, 0, &size);
    require(size > 0 && size <= 64 * 1024, "Cannot read Windows file security: " + path);
    QByteArray descriptor(static_cast<qsizetype>(size), '\0');
    require(GetFileSecurityW(reinterpret_cast<LPCWSTR>(path.utf16()), fields, descriptor.data(), size, &size), "Cannot inspect Windows file security: " + path);
    descriptor.resize(size);
    return descriptor;
#else
    const QByteArray name = QFile::encodeName(path);
    struct stat st{};
    require(::lstat(name.constData(), &st) == 0 && S_ISREG(st.st_mode), "Cannot inspect regular file metadata: " + path);
    require(st.st_uid == ::geteuid() && st.st_nlink == 1 && !(st.st_mode & (S_ISUID | S_ISGID | S_ISVTX)), "Other-owner files, hard links and special mode bits are unsupported: " + path);
#ifdef Q_OS_MACOS
    require(st.st_flags == 0, "Special macOS file flags are unsupported: " + path);
    const ssize_t count = ::listxattr(name.constData(), nullptr, 0, XATTR_NOFOLLOW);
    require(count >= 0 && count <= 64 * 1024, "Cannot inspect extended file attributes: " + path);
    if (count > 0) {
        QByteArray names(count, '\0');
        require(::listxattr(name.constData(), names.data(), names.size(), XATTR_NOFOLLOW) == count, "File attributes changed while being inspected.");
        for (const auto &attribute : names.split('\0')) {
            // macOS 27 attaches this OS-managed marker even to freshly created
            // empty files. The OS supplies a new marker for replacement files;
            // no document attributes or Gatekeeper quarantine are discarded.
            require(attribute.isEmpty() || attribute == "com.apple.provenance", "Extended attributes/resource forks are unsupported; no files changed: " + path);
        }
    }
    for (const QString &entry : {path, QFileInfo(path).absolutePath()}) {
        errno = 0;
        acl_t acl = acl_get_file(QFile::encodeName(entry).constData(), ACL_TYPE_EXTENDED);
        if (!acl && errno == ENOENT && QFileInfo::exists(entry)) continue;
        require(acl != nullptr, "Cannot inspect file access-control list: " + entry);
        acl_entry_t first;
        const int hasEntry = acl_get_entry(acl, ACL_FIRST_ENTRY, &first);
        acl_free(acl);
        require(hasEntry == -1, "Extended access-control lists are unsupported; no files changed: " + entry);
    }
#else
    const ssize_t count = ::llistxattr(name.constData(), nullptr, 0);
    require(count == 0, "Extended attributes/access-control lists are unsupported; no files changed: " + path);
    const QByteArray parent = QFile::encodeName(QFileInfo(path).absolutePath());
    const ssize_t parentCount = ::llistxattr(parent.constData(), nullptr, 0);
    require(parentCount >= 0, "Cannot inspect parent directory metadata.");
    if (parentCount > 0) {
        QByteArray names(parentCount, '\0');
        require(::llistxattr(parent.constData(), names.data(), names.size()) == parentCount, "Parent directory metadata changed.");
        require(!names.contains("system.posix_acl_"), "Parent directory access-control lists are unsupported: " + QFileInfo(path).absolutePath());
    }
#endif
    return QByteArray::number(st.st_uid) + ':' + QByteArray::number(st.st_gid);
#endif
}
void applyNativeSecurity(QFileDevice &output, const Snapshot *metadata) {
#ifdef Q_OS_WIN
    if (!metadata) return; // Backups inherit the private transaction directory ACL.
    require(!metadata->nativeSecurity.isEmpty() && IsValidSecurityDescriptor(const_cast<char *>(metadata->nativeSecurity.constData())), "Missing or invalid Windows security descriptor.");
    const auto originalHandle = reinterpret_cast<HANDLE>(_get_osfhandle(static_cast<int>(output.handle())));
    const HANDLE handle = ReOpenFile(originalHandle, READ_CONTROL | WRITE_DAC | WRITE_OWNER,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
    require(handle != INVALID_HANDLE_VALUE, "Cannot open temporary file to preserve Windows owner and ACL; no replacement performed.");
    SECURITY_DESCRIPTOR_CONTROL control = 0; DWORD revision = 0;
    auto descriptor = reinterpret_cast<PSECURITY_DESCRIPTOR>(const_cast<char *>(metadata->nativeSecurity.constData()));
    const bool inspected = GetSecurityDescriptorControl(descriptor, &control, &revision);
    SECURITY_INFORMATION fields = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    fields |= (control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION;
    const bool applied = inspected && SetKernelObjectSecurity(handle, fields, descriptor);
    CloseHandle(handle);
    require(applied, "Cannot preserve Windows owner and ACL; no replacement performed.");
#else
    if (!metadata) return;
    const auto ids = metadata->nativeSecurity.split(':');
    bool userOk = false, groupOk = false;
    const auto user = ids.value(0).toULongLong(&userOk);
    const auto group = ids.value(1).toULongLong(&groupOk);
    require(ids.size() == 2 && userOk && groupOk && user == ::geteuid(), "Invalid or unsupported Unix ownership metadata.");
    require(::fchown(static_cast<int>(output.handle()), static_cast<uid_t>(user), static_cast<gid_t>(group)) == 0, "Cannot preserve file ownership; no replacement performed.");
#endif
}
void privateMkdir(const QString &path) {
    safePath(path, true);
    noDirectoryAcl(QFileInfo(path).absolutePath());
    require(!QFileInfo::exists(path), "Refusing to reuse a transaction directory: " + path);
#ifdef Q_OS_WIN
    HANDLE token = nullptr;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token), "Cannot read current Windows account.");
    DWORD count = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &count);
    QByteArray bytes(static_cast<qsizetype>(count), '\0');
    const bool tokenOk = GetTokenInformation(token, TokenUser, bytes.data(), count, &count);
    CloseHandle(token);
    require(tokenOk, "Cannot determine current Windows account.");
    LPWSTR sid = nullptr;
    require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(bytes.data())->User.Sid, &sid), "Cannot determine Windows account SID.");
    const QString sddl = QStringLiteral("D:P(A;OICI;FA;;;%1)").arg(QString::fromWCharArray(sid));
    LocalFree(sid);
    PSECURITY_DESCRIPTOR sd = nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<LPCWSTR>(sddl.utf16()), SDDL_REVISION_1, &sd, nullptr), "Cannot create private backup permissions.");
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), sd, FALSE};
    const bool made = CreateDirectoryW(reinterpret_cast<LPCWSTR>(path.utf16()), &attributes);
    LocalFree(sd);
    require(made, "Cannot create private directory: " + path);
#else
    require(::mkdir(QFile::encodeName(path).constData(), 0700) == 0, "Cannot create private directory: " + path);
#endif
}
void syncFile(QFileDevice &file) {
    require(file.flush(), "Failed to flush file: " + file.errorString());
#ifdef Q_OS_WIN
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(static_cast<int>(file.handle())));
    require(handle != INVALID_HANDLE_VALUE && FlushFileBuffers(handle), "Failed to flush file to disk.");
#else
    require(::fsync(static_cast<int>(file.handle())) == 0, "Failed to flush file to disk.");
#endif
}
void syncDirectory(const QString &path) {
#ifndef Q_OS_WIN
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY);
    require(fd >= 0, "Cannot open directory for flush: " + path);
    const int status = ::fsync(fd);
    ::close(fd);
    require(status == 0, "Cannot flush directory: " + path);
#else
    Q_UNUSED(path); // Windows has no equivalent portable directory flush promise.
#endif
}
Snapshot capture(const QString &path, const Options &o, QByteArray *prefix = nullptr, qint64 prefixLimit = 0) {
    checkpoint(o, "before_hash");
    safePath(path);
    localFilesystem(path);
    QFileInfo before(path);
    require(before.isFile(), "Only regular files are supported: " + path);
    require(before.size() <= o.maxFileBytes && before.size() >= 0, "File exceeds the hashing limit: " + path);
    const QByteArray security = nativeMetadata(path);
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot read file: " + path + ": " + file.errorString());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 count = 0;
    while (!file.atEnd()) {
        checkpoint(o, "hash_chunk");
        const QByteArray bytes = file.read(256 * 1024);
        require(file.error() == QFileDevice::NoError, "File read failed: " + path);
        require(!bytes.isEmpty() || file.atEnd(), "File read made no progress: " + path);
        count += bytes.size();
        require(count <= o.maxFileBytes, "File grew beyond hashing limit: " + path);
        hash.addData(bytes);
        if (prefix && prefix->size() < prefixLimit) prefix->append(bytes.left(prefixLimit - prefix->size()));
    }
    safePath(path);
    const QFileInfo after(path);
    require(before.size() == after.size() && before.lastModified() == after.lastModified() && before.permissions() == after.permissions() && count == after.size() && security == nativeMetadata(path), "File changed while being read: " + path);
    return {clean(path), count, hash.result(), after.permissions(), after.lastModified().toUTC(), security};
}
bool contentEquals(const Snapshot &a, const Snapshot &b) { return a.size == b.size && a.sha256 == b.sha256; }
bool snapshotEquals(const Snapshot &a, const Snapshot &b) {
    return contentEquals(a, b) && a.permissions == b.permissions && a.modifiedUtc == b.modifiedUtc && a.nativeSecurity == b.nativeSecurity;
}
void verify(const Snapshot &expected, const Options &o, bool timestamp = true) {
    const Snapshot now = capture(expected.path, o);
    require(contentEquals(now, expected) && now.permissions == expected.permissions && now.nativeSecurity == expected.nativeSecurity && (!timestamp || now.modifiedUtc == expected.modifiedUtc), "File changed since review; rescan required: " + expected.path);
}
void absent(const QString &path) {
    safePath(path, true);
    require(!QFileInfo::exists(path), "Destination already exists; refusing to overwrite: " + path);
}
QJsonObject encode(const Snapshot &s) {
    return {{"path", s.path}, {"size", QString::number(s.size)}, {"sha256", QString::fromLatin1(s.sha256.toHex())}, {"permissions", int(s.permissions)}, {"modifiedUtc", s.modifiedUtc.toString(Qt::ISODateWithMs)}, {"nativeSecurity", QString::fromLatin1(s.nativeSecurity.toBase64())}};
}
Snapshot decode(const QJsonObject &o) {
    Snapshot s;
    s.path = o.value("path").toString();
    bool sizeOk = false;
    s.size = o.value("size").toString().toLongLong(&sizeOk);
    const QByteArray hex = o.value("sha256").toString().toLatin1();
    require(QRegularExpression("^[0-9a-f]{64}$").match(QString::fromLatin1(hex)).hasMatch() && sizeOk && s.size >= 0, "Malformed receipt snapshot.");
    s.sha256 = QByteArray::fromHex(hex);
    s.permissions = QFileDevice::Permissions(o.value("permissions").toInt());
    s.modifiedUtc = QDateTime::fromString(o.value("modifiedUtc").toString(), Qt::ISODateWithMs);
    s.nativeSecurity = QByteArray::fromBase64(o.value("nativeSecurity").toString().toLatin1());
    require(s.modifiedUtc.isValid() && QDir::isAbsolutePath(s.path) && s.path == clean(s.path), "Malformed or non-normalized receipt path or time.");
    return s;
}
void writeManifest(const QString &path, const QJsonObject &manifest) {
    safePath(path, true);
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    require(file.open(QIODevice::WriteOnly), "Cannot write recovery record: " + file.errorString());
    require(file.setPermissions(privateFile), "Cannot restrict recovery record permissions.");
    const QByteArray bytes = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    require(file.write(bytes) == bytes.size(), "Cannot write recovery record: " + file.errorString());
    syncFile(file);
    require(file.commit(), "Cannot commit recovery record: " + file.errorString());
    syncDirectory(QFileInfo(path).absolutePath());
}
QJsonObject readManifest(const QString &path) {
    safePath(path);
    QFile file(path);
    require(file.size() <= 1024 * 1024 && file.open(QIODevice::ReadOnly), "Cannot read recovery record.");
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    require(error.error == QJsonParseError::NoError && doc.isObject() && doc.object().value("format").toString() == "conflictbench-1", "Unsupported or damaged recovery record.");
    return doc.object();
}
QString backupValidation(const ConflictPair &pair, const QString &input) {
    const QString backup = safeDirectory(input);
    localFilesystem(backup);
    noDirectoryAcl(backup);
    require(!within(backup, pair.root), "Backup directory must be outside the scanned folder.");
    const QString syncRoot = markerAncestor(QFileInfo(pair.original.path).absolutePath());
    require(syncRoot.isEmpty() || !within(backup, syncRoot), "Backup directory is inside the source Syncthing tree (.stfolder).");
    require(markerAncestor(backup).isEmpty(), "Backup directory is inside a Syncthing tree (.stfolder).");
    return backup;
}
void validatePair(const ConflictPair &pair) {
    require(safeDirectory(pair.root) == pair.root, "Source root changed or is not canonical.");
    require(within(pair.original.path, pair.root) && within(pair.conflict.path, pair.root), "Files must be inside the selected root.");
    require(QFileInfo(pair.original.path).absolutePath() == QFileInfo(pair.conflict.path).absolutePath(), "A conflict pair must share a directory.");
    require(pair.original.path == clean(pair.original.path) && pair.conflict.path == clean(pair.conflict.path), "Non-normalized source paths are unsupported.");
    require(originalNameForConflict(QFileInfo(pair.conflict.path).fileName()) == QFileInfo(pair.original.path).fileName(), "Conflict filename does not exactly match the original.");
    safePath(pair.original.path);
    safePath(pair.conflict.path);
}
std::unique_ptr<QLockFile> lockTransactions(const Options &o) {
    checkpoint(o, "before_lock");
    // A single per-user lock deliberately serializes even overlapping selected
    // roots and case aliases, independently of the chosen backup directory.
    QString directory = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    require(!directory.isEmpty(), "No local application data directory is available.");
    require(QDir().mkpath(directory), "Cannot create local application data directory.");
    directory = QFileInfo(directory).canonicalFilePath();
    safePath(directory);
    directory = QDir(directory).filePath("conflictbench-locks");
    if (!QFileInfo::exists(directory)) privateMkdir(directory);
    safeDirectory(directory);
    auto lock = std::make_unique<QLockFile>(QDir(directory).filePath("transactions.lock"));
    lock->setStaleLockTime(0);
    require(lock->tryLock(0), "Another ConflictBench transaction is active, or the local lock is unavailable.");
    return lock;
}
void copySnapshot(const Snapshot &source, const QString &target, QFileDevice::Permissions permissions,
                  const Options &o, const std::function<void()> &beforeCommit, const Snapshot *metadata = nullptr) {
    verify(source, o);
    safePath(target, true);
    QSaveFile output(target);
    output.setDirectWriteFallback(false);
    require(output.open(QIODevice::WriteOnly), "Cannot prepare destination: " + target + ": " + output.errorString());
    applyNativeSecurity(output, metadata);
    require(output.setPermissions(permissions), "Cannot set destination permissions: " + target);
    QFile input(source.path);
    require(input.open(QIODevice::ReadOnly), "Cannot read source: " + source.path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 total = 0;
    while (!input.atEnd()) {
        checkpoint(o, "copy_chunk");
        const QByteArray bytes = input.read(256 * 1024);
        require(input.error() == QFileDevice::NoError, "Source read failed.");
        total += bytes.size();
        require(total <= o.maxFileBytes, "Source grew while copying.");
        hash.addData(bytes);
        require(output.write(bytes) == bytes.size(), "Destination write failed (space or permissions): " + output.errorString());
    }
    require(total == source.size && hash.result() == source.sha256, "Source changed while copying.");
    syncFile(output);
    checkpoint(o, "before_copy_commit");
    verify(source, o);
    beforeCommit();
    safePath(target, true);
    require(output.commit(), "Atomic destination commit failed: " + output.errorString());
    syncDirectory(QFileInfo(target).absolutePath());
}
QString bothName(const ConflictPair &pair) {
    QFileInfo info(pair.original.path);
    const QString name = info.fileName();
    const int dot = name.lastIndexOf('.');
    const bool hasExtension = dot > 0;
    const QString stem = hasExtension ? name.left(dot) : name;
    const QString extension = hasExtension ? name.mid(dot) : QString();
    const QString suffix = ".kept-" + QString::fromLatin1(pair.conflict.sha256.toHex().left(12));
    require((stem + suffix + extension).toUtf8().size() <= 240, "Keep-both filename would be too long; use an external manual workflow.");
    return QDir(info.absolutePath()).filePath(stem + suffix + extension);
}
void updateState(QJsonObject &m, const QString &receipt, const QString &state) {
    m.insert("state", state);
    m.insert("updatedUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    writeManifest(receipt, m);
}
Snapshot backupSnapshot(const QString &path, const Snapshot &expected, const Options &o) {
    const auto actual = capture(path, o);
    require(contentEquals(actual, expected), "Backup is corrupt or changed: " + path);
    return actual;
}
} // namespace

QString originalNameForConflict(const QString &name) {
    // Syncthing inserts the marker before filepath.Ext (the final extension).
    static const QRegularExpression pattern(QStringLiteral("^(.*)\\.sync-conflict-([0-9]{8})-([0-9]{6})-([A-Z2-7]{7})(\\.[^./\\\\]*)?$"));
    const auto match = pattern.match(name);
    if (!match.hasMatch() || !QDate::fromString(match.captured(2), "yyyyMMdd").isValid() || !QTime::fromString(match.captured(3), "HHmmss").isValid()) return {};
    const QString original = match.captured(1) + match.captured(5);
    static const QRegularExpression unsafe(QStringLiteral("[\\x00-\\x1f\\x7f<>:\"/\\\\|?*]"));
    static const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)"), QRegularExpression::CaseInsensitiveOption);
    if (original.isEmpty() || original == "." || original == ".." || original.endsWith('.') || original.endsWith(' ') ||
        unsafe.match(original).hasMatch() || unsafe.match(name).hasMatch() || reserved.match(original).hasMatch() ||
        original == ".stignore" || original == ".stfolder" || original == ".stversions" ||
        original.startsWith(".syncthing.") || original.startsWith("~syncthing~")) return {};
    return original;
}
QString actionName(Action action) {
    switch (action) {
    case Action::KeepOriginal: return "Keep original";
    case Action::UseConflict: return "Use conflict";
    case Action::KeepBoth: return "Keep both";
    }
    return "Unknown";
}
ScanResult scan(const QString &input, const Options &o) {
    ScanResult result;
    try {
        result.root = safeDirectory(input);
        localFilesystem(result.root);
        require(result.root != QFileInfo(QDir::homePath()).canonicalFilePath() && !QDir(result.root).isRoot(), "Choose a specific folder; scanning the entire home or a drive root is disabled.");
        require(o.maxFiles > 0 && o.maxConflicts > 0 && o.maxFileBytes >= 0, "Invalid scan limits.");
        QStringList pending{result.root};
        int entries = 0;
        auto warn = [&](const QString &message) {
            if (result.warnings.size() < 200) result.warnings.append(message);
            else if (result.warnings.size() == 200) result.warnings.append("Additional warnings omitted; select a smaller folder for detailed review.");
        };
        while (!pending.isEmpty()) {
            checkpoint(o, "scan_directory");
            const QString directory = pending.takeLast();
            safeDirectory(directory);
            QDir dir(directory);
            require(dir.isReadable(), "Cannot read directory: " + directory);
            QList<QFileInfo> list;
            QSet<QString> exactNames;
            QDirIterator iterator(directory, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
            while (iterator.hasNext()) {
                checkpoint(o, "scan_entry");
                require(++entries <= o.maxFiles, "Scan entry limit reached; select a smaller folder.");
                iterator.next();
                const auto info = iterator.fileInfo();
                list.append(info); exactNames.insert(info.fileName());
                if (o.progress && entries % 32 == 0) o.progress(entries, info.absoluteFilePath());
            }
            for (const auto &info : list) {
                checkpoint(o, "scan_entry");
                try { safePath(info.absoluteFilePath()); }
                catch (const Failure &f) { warn(f.message); continue; }
                if (info.isDir()) {
                    if (info.fileName() != ".stfolder" && info.fileName() != ".stversions" && info.fileName() != ".git") pending.append(info.absoluteFilePath());
                    continue;
                }
                const QString originalName = originalNameForConflict(info.fileName());
                if (originalName.isEmpty()) continue;
                if (!exactNames.contains(originalName)) {
                    warn("Original missing or letter case differs; manual review required: " + info.absoluteFilePath());
                    continue;
                }
                require(result.pairs.size() < o.maxConflicts, "Conflict limit reached; select a smaller folder.");
                try {
                    ConflictPair pair{result.root, capture(dir.filePath(originalName), o), capture(info.absoluteFilePath(), o)};
                    result.pairs.append(pair);
                } catch (const Failure &f) { warn(f.message); }
            }
        }
    } catch (const Failure &f) { result.error = f.message; result.cancelled = o.cancelled && o.cancelled(); }
    return result;
}
Preview preview(const Snapshot &snapshot, qint64 maxBytes, const Options &o) {
    Preview p;
    try {
        require(maxBytes >= 0 && maxBytes <= 16 * 1024 * 1024, "Preview byte limit must be between zero and 16 MiB.");
        const auto now = capture(snapshot.path, o, &p.bytes, maxBytes);
        require(snapshotEquals(now, snapshot), "File changed since scan; rescan before previewing.");
        p.truncated = snapshot.size > maxBytes;
    } catch (const Failure &f) { p.bytes.clear(); p.error = f.message; }
    return p;
}
Plan plan(const ConflictPair &pair, Action action, const QString &backupRoot) {
    Plan p{pair, action, {}, {}, {}, {}};
    try {
        require(action == Action::KeepOriginal || action == Action::UseConflict || action == Action::KeepBoth, "Unknown action.");
        validatePair(pair);
        p.backupRoot = backupValidation(pair, backupRoot);
        if (action == Action::KeepBoth) { p.keepBothPath = bothName(pair); absent(p.keepBothPath); }
        p.description = QString("Back up both versions to a new private directory under:\n%1\n\n%2\nOriginal: %3\nConflict: %4\n").arg(p.backupRoot, actionName(action), pair.original.path, pair.conflict.path);
        if (action == Action::UseConflict) p.description += "Replace the original bytes with the conflict bytes, preserving original permission bits.\n";
        if (action == Action::KeepBoth) p.description += "Create: " + p.keepBothPath + "\n";
        p.description += "Remove the reviewed conflict filename only after verified backups.\nLocal undo is conditional on unchanged files. Pause Syncthing and all editors first.";
    } catch (const Failure &f) { p.error = f.message; }
    return p;
}
Result execute(const Plan &p, bool paused, const Options &o) {
    Result result;
    QJsonObject manifest;
    try {
        require(paused, "Pause Syncthing and all other writers, then explicitly acknowledge before applying.");
        require(p.error.isEmpty(), p.error);
        auto lock = lockTransactions(o);
        const Plan checked = plan(p.pair, p.action, p.backupRoot);
        require(checked.error.isEmpty(), checked.error);
        require(checked.keepBothPath == p.keepBothPath, "The reviewed destination no longer matches the plan.");
        verify(p.pair.original, o); verify(p.pair.conflict, o);
        checkpoint(o, "before_backup_directory");
        const QString directory = QDir(p.backupRoot).filePath("conflictbench-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
        privateMkdir(directory);
        syncDirectory(p.backupRoot);
        result.receiptPath = QDir(directory).filePath("receipt.json");
        manifest = {{"format", "conflictbench-1"}, {"root", p.pair.root}, {"action", int(p.action)}, {"original", encode(p.pair.original)}, {"conflict", encode(p.pair.conflict)}, {"keepBothPath", p.keepBothPath}, {"createdUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
        updateState(manifest, result.receiptPath, "backing_up");
        const QString originalBackup = QDir(directory).filePath("original.backup");
        const QString conflictBackup = QDir(directory).filePath("conflict.backup");
        copySnapshot(p.pair.original, originalBackup, privateFile, o, [&] { absent(originalBackup); });
        copySnapshot(p.pair.conflict, conflictBackup, privateFile, o, [&] { absent(conflictBackup); });
        backupSnapshot(originalBackup, p.pair.original, o);
        backupSnapshot(conflictBackup, p.pair.conflict, o);
        checkpoint(o, "after_backups");
        verify(p.pair.original, o); verify(p.pair.conflict, o);
        updateState(manifest, result.receiptPath, "prepared");
        Snapshot currentOriginal = p.pair.original;
        Snapshot both;
        if (p.action == Action::UseConflict || p.action == Action::KeepBoth) {
            checkpoint(o, "before_destination");
            updateState(manifest, result.receiptPath, "writing_destination");
            const QString target = p.action == Action::UseConflict ? p.pair.original.path : p.keepBothPath;
            copySnapshot(p.pair.conflict, target, p.action == Action::UseConflict ? p.pair.original.permissions : p.pair.conflict.permissions, o, [&] {
                verify(p.pair.original, o); verify(p.pair.conflict, o);
                if (p.action == Action::KeepBoth) absent(p.keepBothPath);
                checkpoint(o, "destination_commit");
                verify(p.pair.original, o); verify(p.pair.conflict, o);
                if (p.action == Action::KeepBoth) absent(p.keepBothPath);
            }, p.action == Action::UseConflict ? &p.pair.original : &p.pair.conflict);
            if (p.action == Action::UseConflict) currentOriginal = capture(p.pair.original.path, o);
            else both = capture(p.keepBothPath, o);
            require(contentEquals(p.action == Action::UseConflict ? currentOriginal : both, p.pair.conflict), "Destination verification failed; recovery required.");
            manifest.insert("postOriginal", encode(currentOriginal));
            if (p.action == Action::KeepBoth) manifest.insert("postBoth", encode(both));
            updateState(manifest, result.receiptPath, "destination_written");
            checkpoint(o, "after_destination");
        }
        checkpoint(o, "before_remove_conflict");
        manifest.insert("postOriginal", encode(currentOriginal));
        updateState(manifest, result.receiptPath, "removing_conflict");
        verify(currentOriginal, o); verify(p.pair.conflict, o);
        if (p.action == Action::KeepBoth) verify(both, o);
        backupSnapshot(originalBackup, p.pair.original, o); backupSnapshot(conflictBackup, p.pair.conflict, o);
        checkpoint(o, "remove_conflict");
        verify(currentOriginal, o); verify(p.pair.conflict, o);
        if (p.action == Action::KeepBoth) verify(both, o);
        require(QFile::remove(p.pair.conflict.path), "Cannot remove reviewed conflict file; recover using history.");
        syncDirectory(QFileInfo(p.pair.conflict.path).absolutePath());
        checkpoint(o, "after_remove_conflict");
        updateState(manifest, result.receiptPath, "committed");
        result.ok = true; result.state = "committed";
    } catch (const Failure &f) {
        result.error = f.message;
        result.state = result.receiptPath.isEmpty() ? "not_started" : "interrupted";
        // Do not overwrite the last durable stage on failure. It may be the only
        // useful evidence if disk-full caused this interruption.
    }
    return result;
}
QVector<Receipt> history(const QString &backupRoot) {
    QVector<Receipt> result;
    try {
        const QString directory = safeDirectory(backupRoot);
        const auto list = QDir(directory).entryInfoList({"conflictbench-*"}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
        for (const auto &entry : list) {
            if (result.size() >= 1000) break;
            Receipt receipt; receipt.path = QDir(entry.absoluteFilePath()).filePath("receipt.json");
            try {
                const auto m = readManifest(receipt.path);
                receipt.root = m.value("root").toString();
                receipt.originalPath = m.value("original").toObject().value("path").toString();
                receipt.conflictPath = m.value("conflict").toObject().value("path").toString();
                receipt.keepBothPath = m.value("keepBothPath").toString();
                receipt.state = m.value("state").toString();
                receipt.action = actionName(static_cast<Action>(m.value("action").toInt(-1)));
                receipt.createdUtc = m.value("createdUtc").toString();
            } catch (const Failure &f) { receipt.error = f.message; receipt.state = "unreadable"; }
            result.append(receipt);
        }
    } catch (const Failure &f) { Receipt r; r.error = f.message; r.state = "unreadable"; result.append(r); }
    return result;
}
Result undo(const QString &receiptPath, bool paused, const Options &o) {
    Result result; result.receiptPath = receiptPath;
    try {
        require(paused, "Pause Syncthing and all other writers, then explicitly acknowledge before undo.");
        auto lock = lockTransactions(o);
        auto m = readManifest(receiptPath);
        const QString state = m.value("state").toString();
        require(state != "undone", "This transaction was already undone.");
        const QSet<QString> states{"backing_up", "prepared", "writing_destination", "destination_written", "removing_conflict", "committed", "undo_started", "undo_conflict_restored", "undo_original_restored"};
        require(states.contains(state), "Unrecognized transaction stage; manual recovery required.");
        ConflictPair pair{m.value("root").toString(), decode(m.value("original").toObject()), decode(m.value("conflict").toObject())};
        const int actionValue = m.value("action").toInt(-1);
        require(actionValue >= 0 && actionValue <= 2, "Invalid receipt action.");
        const auto action = static_cast<Action>(actionValue);
        const QString both = m.value("keepBothPath").toString();
        const QString directory = safeDirectory(QFileInfo(receiptPath).absolutePath());
        require(QFileInfo(receiptPath).fileName() == "receipt.json" && QFileInfo(directory).fileName().startsWith("conflictbench-"), "Unexpected recovery record location.");
        backupValidation(pair, QFileInfo(directory).absolutePath());
        require(safeDirectory(pair.root) == pair.root && within(pair.original.path, pair.root) && within(pair.conflict.path, pair.root), "Receipt source paths are outside the selected root.");
        require(QFileInfo(pair.original.path).absolutePath() == QFileInfo(pair.conflict.path).absolutePath() && originalNameForConflict(QFileInfo(pair.conflict.path).fileName()) == QFileInfo(pair.original.path).fileName(), "Receipt pair does not match conflict naming rules.");
        require(action != Action::KeepBoth || both == bothName(pair), "Receipt keep-both destination is invalid.");
        require(action == Action::KeepBoth || both.isEmpty(), "Unexpected keep-both path.");
        safePath(pair.original.path); safePath(pair.conflict.path, true);
        if (!both.isEmpty()) safePath(both, true);
        auto originalNow = capture(pair.original.path, o);
        const bool originalIsOld = contentEquals(originalNow, pair.original);
        require(originalNow.permissions == pair.original.permissions && originalNow.nativeSecurity == pair.original.nativeSecurity && (originalIsOld || (action == Action::UseConflict && contentEquals(originalNow, pair.conflict))), "Original has later changes; undo refused.");
        const bool conflictExists = QFileInfo::exists(pair.conflict.path);
        Snapshot conflictNow;
        if (conflictExists) { conflictNow = capture(pair.conflict.path, o); require(contentEquals(conflictNow, pair.conflict) && conflictNow.permissions == pair.conflict.permissions && conflictNow.nativeSecurity == pair.conflict.nativeSecurity, "Conflict has later changes; undo refused."); }
        const bool bothExists = !both.isEmpty() && QFileInfo::exists(both);
        Snapshot bothNow;
        if (bothExists) { bothNow = capture(both, o); require(contentEquals(bothNow, pair.conflict) && bothNow.permissions == pair.conflict.permissions && bothNow.nativeSecurity == pair.conflict.nativeSecurity, "Kept copy has later changes; undo refused."); }
        const bool undoInProgress = state.startsWith("undo_");
        if (state == "backing_up" || state == "prepared") {
            require(snapshotEquals(originalNow, pair.original) && conflictExists && snapshotEquals(conflictNow, pair.conflict) && !bothExists,
                    "Files changed after preparation; recovery refuses to overwrite later changes.");
        } else if (state == "writing_destination") {
            require(action == Action::UseConflict || action == Action::KeepBoth, "Invalid destination stage for action.");
            // A rename may have completed before its post-state was recorded.
            // Matching bytes cannot prove whether it was our write or a later
            // user's write. Never delete/replace such an ambiguous destination.
            require(snapshotEquals(originalNow, pair.original) && conflictExists
                        && snapshotEquals(conflictNow, pair.conflict) && !bothExists,
                    "Destination write has no durable post-state. Automatic recovery is refused unless both sources are pristine; preserve backups and recover manually.");
        } else if (!undoInProgress) {
            require(snapshotEquals(originalNow, decode(m.value("postOriginal").toObject())), "Original changed after recorded destination stage; undo refused.");
            if (action == Action::KeepBoth) require(bothExists && snapshotEquals(bothNow, decode(m.value("postBoth").toObject())), "Kept copy changed after recorded destination stage; undo refused.");
            if (state == "destination_written") require(conflictExists && snapshotEquals(conflictNow, pair.conflict), "Conflict changed before removal stage; undo refused.");
            else if (state == "committed") require(!conflictExists, "Conflict filename was recreated after commit; undo refused.");
            else if (conflictExists) require(snapshotEquals(conflictNow, pair.conflict), "Conflict changed during removal stage; undo refused.");
        } else {
            const Snapshot startedOriginal = decode(m.value("undoStartedOriginal").toObject());
            const bool startedConflict = m.value("undoStartedConflictExists").toBool();
            const bool startedBoth = m.value("undoStartedBothExists").toBool();
            if (state == "undo_started") {
                require(snapshotEquals(originalNow, startedOriginal) && bothExists == startedBoth, "Files changed after undo started; recovery refused.");
                if (startedConflict) require(conflictExists && snapshotEquals(conflictNow, decode(m.value("undoStartedConflict").toObject())), "Conflict changed during undo.");
                // Otherwise conflict may still be absent, or have just been
                // restored before the next durable stage was written.
            } else {
                require(conflictExists && snapshotEquals(conflictNow, decode(m.value("undoRestoredConflict").toObject())), "Restored conflict changed during undo.");
                if (state == "undo_conflict_restored") require((snapshotEquals(originalNow, startedOriginal) || originalIsOld) && bothExists == startedBoth, "Unexpected files during original restoration.");
                else require(snapshotEquals(originalNow, decode(m.value("undoRestoredOriginal").toObject())), "Restored original changed during undo.");
            }
            if (bothExists) require(startedBoth && snapshotEquals(bothNow, decode(m.value("undoStartedBoth").toObject())), "Kept copy changed during undo.");
        }
        // An interrupted backup stage has never mutated source files. It can be
        // closed without complete backups, only when both source snapshots match.
        if (state == "backing_up") {
            verify(pair.original, o); verify(pair.conflict, o);
            require(!bothExists, "Unexpected kept copy during backup stage.");
            checkpoint(o, "before_close_unstarted");
            updateState(m, receiptPath, "undone");
            result.ok = true; result.state = "undone"; return result;
        }
        const auto originalBackup = backupSnapshot(QDir(directory).filePath("original.backup"), pair.original, o);
        const auto conflictBackup = backupSnapshot(QDir(directory).filePath("conflict.backup"), pair.conflict, o);
        checkpoint(o, "before_undo");
        m.insert("undoStartedOriginal", encode(originalNow));
        m.insert("undoStartedConflictExists", conflictExists);
        m.insert("undoStartedBothExists", bothExists);
        if (conflictExists) m.insert("undoStartedConflict", encode(conflictNow));
        if (bothExists) m.insert("undoStartedBoth", encode(bothNow));
        updateState(m, receiptPath, "undo_started");
        auto verifyCurrent = [&] {
            verify(originalNow, o);
            if (conflictExists) verify(conflictNow, o); else absent(pair.conflict.path);
            if (bothExists) verify(bothNow, o); else if (!both.isEmpty()) absent(both);
        };
        verifyCurrent();
        if (!conflictExists) {
            copySnapshot(conflictBackup, pair.conflict.path, pair.conflict.permissions, o, verifyCurrent, &pair.conflict);
            conflictNow = capture(pair.conflict.path, o);
        }
        m.insert("undoRestoredConflict", encode(conflictNow));
        updateState(m, receiptPath, "undo_conflict_restored");
        checkpoint(o, "after_undo_conflict");
        if (!originalIsOld) {
            copySnapshot(originalBackup, pair.original.path, pair.original.permissions, o, [&] {
                verify(originalNow, o); verify(conflictNow, o); if (bothExists) verify(bothNow, o);
            }, &pair.original);
            originalNow = capture(pair.original.path, o);
        }
        m.insert("undoRestoredOriginal", encode(originalNow));
        updateState(m, receiptPath, "undo_original_restored");
        checkpoint(o, "after_undo_original");
        if (bothExists) {
            verify(originalNow, o); verify(conflictNow, o); verify(bothNow, o);
            checkpoint(o, "undo_remove_both");
            verify(originalNow, o); verify(conflictNow, o); verify(bothNow, o);
            require(QFile::remove(both), "Cannot remove kept copy; retry local recovery.");
            syncDirectory(QFileInfo(both).absolutePath());
        }
        checkpoint(o, "before_undo_complete");
        updateState(m, receiptPath, "undone");
        result.ok = true; result.state = "undone";
    } catch (const Failure &f) { result.error = f.message; result.state = "recovery_required"; }
    return result;
}
} // namespace conflictbench
