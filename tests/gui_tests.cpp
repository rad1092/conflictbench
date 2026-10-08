#include "workbench.h"
#include <QtTest>
#include <QtWidgets>
#include <QCryptographicHash>
#include <functional>

namespace {
using namespace conflictbench;

template<class T> T *named(Workbench &window, const char *name) {
    return window.findChild<T *>(QString::fromLatin1(name));
}

QLabel *accessibleLabel(Workbench &window, const QString &name) {
    for (auto *label : window.findChildren<QLabel *>())
        if (label->accessibleName() == name) return label;
    return nullptr;
}

QTreeWidgetItem *version(Workbench &window, const QString &original) {
    auto *tree = named<QTreeWidget>(window, "conflicts");
    if (!tree) return nullptr;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        auto *group = tree->topLevelItem(i);
        if (group->text(0) == original && group->childCount()) return group->child(0);
    }
    return nullptr;
}

QString demoRoot(Workbench &window) {
    auto *label = named<QLabel>(window, "folderPath");
    return label ? label->text() : QString{};
}

QString demoBackups(const QString &root) {
    return QFileInfo(root).dir().filePath("Backups");
}

QString demoDiagnostic(Workbench &window) {
    const auto result = scan(demoRoot(window));
    return result.error + '\n' + result.warnings.join('\n');
}

QByteArray contents(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

bool overwrite(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

bool choosePickerPath(QFileDialog *picker, const QString &path) {
    // On Cocoa, selectFile() can clear the selection while the directory model
    // initializes. Enter the absolute path through the same field a user edits.
    auto *entry = picker->findChild<QLineEdit *>("fileNameEdit");
    if (!entry) { picker->reject(); return false; }
    picker->setDirectory(QFileInfo(path).absolutePath());
    entry->setText(QDir::toNativeSeparators(path));
    QMetaObject::invokeMethod(picker, "accept", Qt::QueuedConnection);
    return true;
}

bool captureSyntheticWidget(QWidget &widget, const QString &name) {
    const QString directory = qEnvironmentVariable("CB_GUI_CAPTURE_DIR");
    if (directory.isEmpty()) return true;
    if (!QDir().mkpath(directory)) return false;
    // This captures only our synthetic QWidget, never another app or desktop.
    widget.repaint();
    const QPixmap pixels = widget.grab();
    const QString file = QDir(directory).filePath(
        QGuiApplication::platformName() + '-' + name + ".png");
    const bool saved = !pixels.isNull() && pixels.save(file);
    if (saved) qInfo() << "Synthetic GUI capture" << QFileInfo(file).fileName()
                       << pixels.size() << "devicePixelRatio" << pixels.devicePixelRatio();
    return saved;
}

// Directory entries and byte hashes prove cancel does not write even a receipt.
QMap<QString, QByteArray> directorySnapshot(const QString &root) {
    QMap<QString, QByteArray> result;
    QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QString relative = QDir(root).relativeFilePath(it.filePath());
        result.insert(relative, it.fileInfo().isDir() ? QByteArray("directory")
            : QCryptographicHash::hash(contents(it.filePath()), QCryptographicHash::Sha256));
    }
    return result;
}

struct ReviewObservation {
    bool sawPlan = false;
    bool controlsPresent = false;
    bool initialDisabled = false;
    bool pauseOnlyDisabled = false;
    bool backupOnlyDisabled = false;
    bool bothEnabled = false;
    bool uncheckDisables = false;
    bool timedOut = false;
    bool sawWarning = false;
    bool sawInformation = false;
    bool captureSaved = true;
    QString planText;
    QString comparisonScope;
    QStringList messages;
};

ReviewObservation driveReview(Workbench &window, bool commit,
                              const std::function<void()> &beforeDecision = {}) {
    ReviewObservation observed;
    QTimer timer;
    QElapsedTimer elapsed;
    elapsed.start();
    QObject::connect(&timer, &QTimer::timeout, &window, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (elapsed.elapsed() > 15000) {
            observed.timedOut = true;
            if (dialog) dialog->reject();
            return;
        }
        if (!dialog) return;
        if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
            observed.messages << message->windowTitle() + ": " + message->text();
            observed.sawWarning |= message->icon() == QMessageBox::Warning;
            observed.sawInformation |= message->icon() == QMessageBox::Information;
            message->accept();
            return;
        }
        if (dialog->objectName() != "planDialog" || observed.sawPlan) return;
        observed.sawPlan = true;
        auto *paused = dialog->findChild<QCheckBox *>("pauseAcknowledgment");
        auto *privateBackup = dialog->findChild<QCheckBox *>("backupAcknowledgment");
        auto *button = dialog->findChild<QPushButton *>("commit");
        auto *planText = dialog->findChild<QPlainTextEdit *>("Transaction plan");
        auto *scope = dialog->findChild<QLabel *>("planComparisonScope");
        observed.controlsPresent = paused && privateBackup && button && planText && scope && scope->isVisible();
        if (!observed.controlsPresent) { dialog->reject(); return; }
        observed.planText = planText->toPlainText();
        observed.comparisonScope = scope->text();
        observed.captureSaved = captureSyntheticWidget(*dialog,
            scope->text().startsWith("Partial preview") ? "partial-plan" : "plan");
        observed.initialDisabled = !button->isEnabled();
        paused->setChecked(true);
        observed.pauseOnlyDisabled = !button->isEnabled();
        paused->setChecked(false);
        privateBackup->setChecked(true);
        observed.backupOnlyDisabled = !button->isEnabled();
        paused->setChecked(true);
        observed.bothEnabled = button->isEnabled();
        privateBackup->setChecked(false);
        observed.uncheckDisables = !button->isEnabled();
        privateBackup->setChecked(true);
        if (beforeDecision) beforeDecision();
        if (commit) button->click();
        else dialog->reject();
    });
    timer.start(5);
    auto *review = named<QPushButton>(window, "reviewPlan");
    if (review) QTest::mouseClick(review, Qt::LeftButton);
    timer.stop();
    return observed;
}

struct HistoryObservation {
    bool sawPicker = false;
    bool sawHistory = false;
    bool undoInitiallyDisabled = false;
    bool timedOut = false;
    bool pickerFieldPresent = false;
    bool captureSaved = true;
    int count = -1;
    QString original;
    QString selectedPath;
};

HistoryObservation driveHistory(Workbench &window, const QString &backupRoot) {
    HistoryObservation observed;
    QTimer timer;
    QElapsedTimer elapsed;
    elapsed.start();
    QObject::connect(&timer, &QTimer::timeout, &window, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (elapsed.elapsed() > 15000) {
            observed.timedOut = true;
            if (dialog) dialog->reject();
            return;
        }
        if (auto *picker = qobject_cast<QFileDialog *>(dialog)) {
            if (observed.sawPicker) return;
            observed.sawPicker = true;
            QObject::connect(picker, &QFileDialog::filesSelected, picker,
                             [&](const QStringList &files) { observed.selectedPath = files.value(0); });
            observed.pickerFieldPresent = choosePickerPath(picker, backupRoot);
            return;
        }
        if (!dialog || dialog->windowTitle() != "History / recovery") return;
        observed.sawHistory = true;
        if (auto *list = dialog->findChild<QTreeWidget *>()) {
            observed.count = list->topLevelItemCount();
            if (observed.count) observed.original = list->topLevelItem(0)->text(3);
        }
        for (auto *button : dialog->findChildren<QPushButton *>())
            if (button->text() == "Undo / recover selected")
                observed.undoInitiallyDisabled = !button->isEnabled();
        observed.captureSaved = captureSyntheticWidget(*dialog, "history");
        dialog->reject();
    });
    timer.start(5);
    if (auto *button = named<QPushButton>(window, "history"))
        QTest::mouseClick(button, Qt::LeftButton);
    timer.stop();
    return observed;
}
}

class GuiTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
    }

    void demoShowsTextImageAndBinary() {
        Workbench window;
        window.show();
        auto *demo = named<QPushButton>(window, "demo");
        auto *review = named<QPushButton>(window, "reviewPlan");
        auto *external = named<QPushButton>(window, "externalDiff");
        QVERIFY(demo && review && external);
        QVERIFY(!review->isEnabled());
        QVERIFY(!external->isEnabled());
        demo->setFocus();
        QTest::keyClick(demo, Qt::Key_Space);

        auto *tree = named<QTreeWidget>(window, "conflicts");
        QVERIFY(tree);
        QVERIFY2(tree->topLevelItemCount() == 3, qPrintable(demoDiagnostic(window)));
        auto *textVersion = version(window, QString::fromUtf8("여행 계획.txt"));
        QVERIFY(textVersion);
        tree->setCurrentItem(textVersion);
        auto *left = named<QPlainTextEdit>(window, "Original text preview");
        auto *right = named<QPlainTextEdit>(window, "Conflict text preview");
        auto *diff = named<QPlainTextEdit>(window, "Text difference");
        QVERIFY(left && right && diff);
        QVERIFY(left->isReadOnly() && right->isReadOnly() && diff->isReadOnly());
        QVERIFY(left->toPlainText().contains(QString::fromUtf8("서울 여행")));
        QVERIFY(left->toPlainText().contains("palace at 10:00"));
        QVERIFY(right->toPlainText().contains("museum at 11:00"));
        QVERIFY(diff->toPlainText().contains("- Saturday: palace at 10:00"));
        QVERIFY(diff->toPlainText().contains("+ Saturday: museum at 11:00"));
        QVERIFY(review->isEnabled() && external->isEnabled());
        QVERIFY(captureSyntheticWidget(window, "text"));

        auto *imageVersion = version(window, "design.png");
        QVERIFY(imageVersion);
        tree->setCurrentItem(imageVersion);
        auto *leftImage = accessibleLabel(window, "Original image preview");
        auto *rightImage = accessibleLabel(window, "Conflict image preview");
        QVERIFY(leftImage && rightImage);
        QVERIFY(!leftImage->isHidden() && !rightImage->isHidden());
        QVERIFY(!leftImage->pixmap().isNull() && !rightImage->pixmap().isNull());
        QCOMPARE(leftImage->pixmap().toImage().pixelColor(0, 0), QColor("#326764"));
        QCOMPARE(rightImage->pixmap().toImage().pixelColor(0, 0), QColor("#74512e"));
        QVERIFY(left->isHidden() && right->isHidden());
        QVERIFY(diff->toPlainText().contains("text diff is unavailable"));
        QVERIFY(captureSyntheticWidget(window, "image"));

        auto *binaryVersion = version(window, "budget.v2.dat");
        QVERIFY(binaryVersion);
        tree->setCurrentItem(binaryVersion);
        QVERIFY(leftImage->isHidden() && rightImage->isHidden());
        QVERIFY(!left->isHidden() && !right->isHidden());
        QVERIFY(left->toPlainText().contains("Binary or unsupported text encoding"));
        QVERIFY(right->toPlainText().contains("choose a decision explicitly"));
        QVERIFY(diff->toPlainText().contains("choose explicitly"));
        QVERIFY(captureSyntheticWidget(window, "binary"));

        tree->setCurrentItem(binaryVersion->parent());
        QVERIFY(!review->isEnabled());
        QVERIFY(!external->isEnabled());
        QVERIFY(left->toPlainText().isEmpty() && right->toPlainText().isEmpty());
    }

    void cancelPlanIsReadOnlyAndRequiresBothAcknowledgments() {
        Workbench window;
        window.show();
        window.createDemo();
        const QString root = demoRoot(window);
        QVERIFY(!root.isEmpty());
        const QString parent = QFileInfo(root).dir().absolutePath();
        const auto before = directorySnapshot(parent);
        const auto observed = driveReview(window, false);
        QVERIFY(!observed.timedOut);
        QVERIFY(observed.sawPlan && observed.controlsPresent);
        QVERIFY(observed.captureSaved);
        QVERIFY(observed.initialDisabled);
        QVERIFY(observed.pauseOnlyDisabled);
        QVERIFY(observed.backupOnlyDisabled);
        QVERIFY(observed.bothEnabled);
        QVERIFY(observed.uncheckDisables);
        QVERIFY(observed.planText.contains("SHA-256"));
        QVERIFY(observed.planText.contains("not an atomic multi-file guarantee"));
        QVERIFY(observed.planText.contains(demoBackups(root)));
        QCOMPARE(directorySnapshot(parent), before);
        QVERIFY(history(demoBackups(root)).isEmpty());
    }

    void explicitCommit_data() {
        QTest::addColumn<int>("actionIndex");
        QTest::newRow("keep-original") << 0;
        QTest::newRow("use-conflict") << 1;
        QTest::newRow("keep-both") << 2;
    }

    void explicitCommit() {
        QFETCH(int, actionIndex);
        Workbench window;
        window.show();
        window.createDemo();
        const QString root = demoRoot(window);
        auto scanned = scan(root);
        QVERIFY2(scanned.error.isEmpty(), qPrintable(scanned.error));
        QCOMPARE(scanned.pairs.size(), 3);
        ConflictPair selected;
        for (const auto &pair : scanned.pairs)
            if (QFileInfo(pair.original.path).fileName() == QString::fromUtf8("여행 계획.txt")) selected = pair;
        QVERIFY(!selected.original.path.isEmpty());
        const QByteArray original = contents(selected.original.path);
        const QByteArray conflict = contents(selected.conflict.path);
        auto *decision = named<QComboBox>(window, "decision");
        QVERIFY(decision);
        decision->setCurrentIndex(actionIndex);
        const auto observed = driveReview(window, true);
        QVERIFY(!observed.timedOut);
        QVERIFY(observed.sawPlan && observed.controlsPresent && observed.bothEnabled);
        QVERIFY(observed.captureSaved);
        QVERIFY2(observed.sawInformation && observed.messages.join('\n').contains("State: committed"),
                 qPrintable(observed.messages.join('\n')));
        QVERIFY(!QFile::exists(selected.conflict.path));
        QCOMPARE(contents(selected.original.path), actionIndex == 1 ? conflict : original);
        auto receipts = history(demoBackups(root));
        QCOMPARE(receipts.size(), 1);
        QVERIFY(QFile::exists(receipts.first().path));
        QCOMPARE(receipts.first().originalPath, selected.original.path);
        if (actionIndex == 2) {
            QVERIFY(!receipts.first().keepBothPath.isEmpty());
            QCOMPARE(contents(receipts.first().keepBothPath), conflict);
        }
        QCOMPARE(scan(root).pairs.size(), 2);
        QCOMPARE(named<QTreeWidget>(window, "conflicts")->topLevelItemCount(), 2);

        const auto displayed = driveHistory(window, demoBackups(root));
        QVERIFY(!displayed.timedOut);
        QVERIFY(displayed.sawPicker && displayed.sawHistory);
        QVERIFY(displayed.pickerFieldPresent && displayed.captureSaved);
        QCOMPARE(QFileInfo(displayed.selectedPath).canonicalFilePath(),
                 QFileInfo(demoBackups(root)).canonicalFilePath());
        QCOMPARE(displayed.count, 1);
        QCOMPARE(displayed.original, QString::fromUtf8("여행 계획.txt"));
        QVERIFY(displayed.undoInitiallyDisabled);

        // Core rollback cleans up the test transaction and proves the receipt
        // saved by the UI is consumable; GUI rollback remains manual coverage.
        auto restored = undo(receipts.first().path, true);
        QVERIFY2(restored.ok, qPrintable(restored.error));
        QCOMPARE(contents(selected.original.path), original);
        QCOMPARE(contents(selected.conflict.path), conflict);
    }

    void sameSizeChangeInvalidatesPreview() {
        Workbench window;
        window.show();
        window.createDemo();
        auto *tree = named<QTreeWidget>(window, "conflicts");
        auto *selected = version(window, QString::fromUtf8("여행 계획.txt"));
        QVERIFY2(tree && selected, qPrintable(demoDiagnostic(window)));
        const QString original = QDir(demoRoot(window)).filePath(QString::fromUtf8("여행 계획.txt"));
        QByteArray bytes = contents(original);
        QVERIFY(!bytes.isEmpty());
        bytes[0] = bytes[0] == 'S' ? 'X' : 'S';
        QVERIFY(overwrite(original, bytes));
        tree->setCurrentItem(selected->parent());
        tree->setCurrentItem(selected);
        QVERIFY(!named<QPushButton>(window, "reviewPlan")->isEnabled());
        QVERIFY(!named<QPushButton>(window, "externalDiff")->isEnabled());
        QVERIFY(named<QPlainTextEdit>(window, "Text difference")->toPlainText()
                    .contains("Reopen the folder to scan again"));
        QCOMPARE(contents(original), bytes);
    }

    void truncatedTextNeverPresentsMatchingPrefixAsCompleteDifference() {
        Workbench window;
        window.show();
        window.createDemo();
        const QString root = demoRoot(window);
        const QString original = QDir(root).filePath(QString::fromUtf8("여행 계획.txt"));
        const QString conflict = QDir(root).filePath(
            QString::fromUtf8("여행 계획.sync-conflict-20261001-091530-ABCDEFG.txt"));
        QByteArray sharedPrefix(1024 * 1024, 'a');
        // The prefix has fewer than 1,200 lines, so the line-count diff limit
        // cannot hide a regression in the independent 1 MiB byte limit.
        for (int i = 1023; i < sharedPrefix.size(); i += 1024) sharedPrefix[i] = '\n';
        QVERIFY(overwrite(original, sharedPrefix + "OLD\n"));
        QVERIFY(overwrite(conflict, sharedPrefix + "NEW\n"));
        const auto before = directorySnapshot(QFileInfo(root).dir().absolutePath());
        window.scanFolder(root);
        auto *tree = named<QTreeWidget>(window, "conflicts");
        auto *selected = version(window, QString::fromUtf8("여행 계획.txt"));
        QVERIFY(tree && selected);
        tree->setCurrentItem(selected);
        auto *left = named<QPlainTextEdit>(window, "Original text preview");
        auto *right = named<QPlainTextEdit>(window, "Conflict text preview");
        auto *diff = named<QPlainTextEdit>(window, "Text difference");
        auto *scope = named<QLabel>(window, "comparisonScope");
        QVERIFY(left && right && diff && scope);
        QVERIFY(left->toPlainText().startsWith("[Partial preview: first 1 MiB"));
        QVERIFY(right->toPlainText().startsWith("[Partial preview: first 1 MiB"));
        QVERIFY(!left->toPlainText().contains("OLD"));
        QVERIFY(!right->toPlainText().contains("NEW"));
        QVERIFY(diff->toPlainText().contains("Inline text difference is unavailable"));
        QVERIFY(diff->toPlainText().contains("remaining content is not shown"));
        QVERIFY(!diff->toPlainText().contains("--- original"));
        QVERIFY(scope->isVisible());
        QVERIFY(scope->text().contains("Partial preview"));
        QVERIFY(scope->text().contains("Whole-file SHA-256 hashes differ"));
        auto *tabs = window.findChild<QTabWidget *>();
        QVERIFY(tabs);
        tabs->setCurrentWidget(diff);
        QVERIFY(diff->isVisible() && scope->isVisible());
        QVERIFY(captureSyntheticWidget(window, "partial-text-difference"));

        const auto observed = driveReview(window, false);
        QVERIFY(!observed.timedOut && observed.sawPlan && observed.controlsPresent);
        QVERIFY(observed.captureSaved);
        QVERIFY(observed.comparisonScope.contains("Partial preview"));
        QVERIFY(observed.comparisonScope.contains("1 MiB"));
        QVERIFY(observed.comparisonScope.contains("Whole-file SHA-256 hashes differ"));
        QVERIFY(observed.comparisonScope.contains("decision applies to the whole files"));
        QCOMPARE(directorySnapshot(QFileInfo(root).dir().absolutePath()), before);
        QVERIFY(history(demoBackups(root)).isEmpty());
    }

    void committingLastScanEntryRescansWithoutStaleSelection() {
        Workbench window;
        window.show();
        window.createDemo();
        const QString root = demoRoot(window);
        const auto scanned = scan(root);
        QVERIFY2(scanned.error.isEmpty(), qPrintable(scanned.error));
        QCOMPARE(scanned.pairs.size(), 3);
        const int lastIndex = scanned.pairs.size() - 1;
        const auto selected = scanned.pairs.last();
        auto *tree = named<QTreeWidget>(window, "conflicts");
        QVERIFY(tree);
        QTreeWidgetItem *lastVersion = nullptr;
        QTreeWidgetItemIterator item(tree);
        while (*item) {
            if ((*item)->parent() && (*item)->data(0, Qt::UserRole).toInt() == lastIndex)
                lastVersion = *item;
            ++item;
        }
        QVERIFY(lastVersion);
        tree->setCurrentItem(lastVersion);
        auto *decision = named<QComboBox>(window, "decision");
        QVERIFY(decision);
        decision->setCurrentIndex(0);
        auto expected = directorySnapshot(root);
        QCOMPARE(expected.remove(QDir(root).relativeFilePath(selected.conflict.path)), 1);

        const auto observed = driveReview(window, true);
        QVERIFY(!observed.timedOut && observed.sawPlan && observed.controlsPresent);
        QVERIFY(observed.captureSaved);
        QVERIFY2(observed.sawInformation && observed.messages.join('\n').contains("State: committed"),
                 qPrintable(observed.messages.join('\n')));
        QCOMPARE(directorySnapshot(root), expected);
        QCOMPARE(scan(root).pairs.size(), 2);
        QCOMPARE(tree->topLevelItemCount(), 2);
        const auto receipts = history(demoBackups(root));
        QCOMPARE(receipts.size(), 1);
        QCOMPARE(receipts.first().originalPath, selected.original.path);
        // The removed item had index 2 while the new scan has only indices 0/1.
        // Clearing the old tree must not preview its stale index in the new scan.
        QVERIFY(tree->currentItem());
        QVERIFY(tree->currentItem()->parent());
        const int nextIndex = tree->currentItem()->data(0, Qt::UserRole).toInt();
        QVERIFY(nextIndex >= 0 && nextIndex < 2);
        QVERIFY(named<QPushButton>(window, "reviewPlan")->isEnabled());
    }

    void changeDuringReviewRefusesCommit() {
        Workbench window;
        window.show();
        window.createDemo();
        const QString root = demoRoot(window);
        const QString original = QDir(root).filePath(QString::fromUtf8("여행 계획.txt"));
        QByteArray changed = contents(original);
        QVERIFY(!changed.isEmpty());
        changed[0] = 'X';
        const auto conflictsBefore = scan(root).pairs.size();
        bool mutationWritten = false;
        const auto observed = driveReview(window, true, [&] { mutationWritten = overwrite(original, changed); });
        QVERIFY(mutationWritten);
        QVERIFY(!observed.timedOut && observed.sawPlan);
        QVERIFY(observed.captureSaved);
        QVERIFY2(observed.sawWarning && observed.messages.join('\n').contains("rescan required"),
                 qPrintable(observed.messages.join('\n')));
        QVERIFY(!observed.messages.join('\n').contains("State: committed"));
        QCOMPARE(contents(original), changed);
        QCOMPARE(scan(root).pairs.size(), conflictsBefore);
    }

    void leavingDemoRequiresNewBackupSelection() {
        Workbench window;
        window.show();
        window.createDemo();
        const QString oldDemo = QFileInfo(demoRoot(window)).dir().absolutePath();
        const auto demoBefore = directorySnapshot(oldDemo);
        QTemporaryDir secondFolder;
        QVERIFY(secondFolder.isValid());
        const QString secondRoot = QFileInfo(secondFolder.path()).canonicalFilePath();
        QVERIFY(QDir().mkpath(secondRoot + "/.stfolder"));
        QVERIFY(overwrite(secondRoot + "/second.txt", "original\n"));
        QVERIFY(overwrite(secondRoot + "/second.sync-conflict-20261001-091530-ABCDEFG.txt", "conflict\n"));
        window.scanFolder(secondRoot);
        QCOMPARE(named<QTreeWidget>(window, "conflicts")->topLevelItemCount(), 1);
        const auto before = directorySnapshot(secondRoot);

        bool askedForBackup = false;
        bool reusedDisposableBackup = false;
        bool timedOut = false;
        QTimer timer;
        QElapsedTimer elapsed;
        elapsed.start();
        connect(&timer, &QTimer::timeout, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            if (auto *picker = qobject_cast<QFileDialog *>(dialog)) {
                askedForBackup = picker->windowTitle().contains("private backup folder");
                picker->reject();
            } else if (dialog->objectName() == "planDialog") {
                reusedDisposableBackup = true;
                dialog->reject();
            } else if (elapsed.elapsed() > 15000) {
                timedOut = true;
                dialog->reject();
            }
        });
        timer.start(5);
        QTest::mouseClick(named<QPushButton>(window, "reviewPlan"), Qt::LeftButton);
        timer.stop();
        QVERIFY(!timedOut);
        QVERIFY(askedForBackup);
        QVERIFY(!reusedDisposableBackup);
        QCOMPARE(directorySnapshot(secondRoot), before);
        QCOMPARE(directorySnapshot(oldDemo), demoBefore);
    }

    void externalToolFailureLeavesSourceUntouched() {
        Workbench window;
        window.show();
        window.createDemo();
        const QString demoParent = QFileInfo(demoRoot(window)).dir().absolutePath();
        const auto before = directorySnapshot(demoParent);
        QTemporaryDir toolFolder;
        QVERIFY(toolFolder.isValid());
        const QString program = toolFolder.path() + "/not an executable.exe";
        QVERIFY(overwrite(program, "This synthetic file is not an executable.\n"));

        bool sawPicker = false;
        bool sawArguments = false;
        bool sawFailure = false;
        bool pickerFieldPresent = false;
        bool timedOut = false;
        QString selectedProgram;
        QStringList messages;
        QTimer timer;
        QElapsedTimer elapsed;
        elapsed.start();
        connect(&timer, &QTimer::timeout, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (elapsed.elapsed() > 15000) {
                timedOut = true;
                if (dialog) dialog->reject();
                return;
            }
            if (auto *picker = qobject_cast<QFileDialog *>(dialog)) {
                if (sawPicker) return;
                sawPicker = true;
                QObject::connect(picker, &QFileDialog::filesSelected, picker,
                                 [&](const QStringList &files) { selectedProgram = files.value(0); });
                pickerFieldPresent = choosePickerPath(picker, program);
            } else if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
                messages << message->windowTitle() + ": " + message->text();
                // macOS intentionally ignores QMessageBox window titles.
                sawFailure |= sawArguments && message->icon() == QMessageBox::Warning;
                message->accept();
            } else if (dialog && dialog->windowTitle() == "External diff arguments") {
                sawArguments = true;
                dialog->accept();
            }
        });
        timer.start(5);
        QTest::mouseClick(named<QPushButton>(window, "externalDiff"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(sawFailure || timedOut, 16000);
        timer.stop();
        QVERIFY(!timedOut);
        QVERIFY(sawPicker && sawArguments && pickerFieldPresent);
        QCOMPARE(QFileInfo(selectedProgram).canonicalFilePath(), QFileInfo(program).canonicalFilePath());
        QVERIFY2(sawFailure, qPrintable(messages.join('\n')));
        QCOMPARE(directorySnapshot(demoParent), before);
    }
};

int main(int argc, char **argv) {
    // Select deterministic Qt file-dialog widgets before Cocoa initializes its
    // integration. The main windows still use the native platform plugin.
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    QApplication application(argc, argv);
    GuiTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "gui_tests.moc"
