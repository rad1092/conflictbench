#pragma once
#include "core.h"
#include <QMainWindow>
#include <QTemporaryDir>
#include <memory>
class QTreeWidget;
class QPlainTextEdit;
class QLabel;
class QComboBox;
class QPushButton;
class QTabWidget;
class QCloseEvent;
class Workbench final : public QMainWindow {
    Q_OBJECT
public:
    explicit Workbench(QWidget *parent = nullptr);
    void createDemo();
    void scanFolder(const QString &folder);
    bool smokeTest(QString *error);
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    void showPair();
    void review();
    void historyDialog();
    void externalDiff();
    void help();
    void cleanIdentical();
    void highlightDifferences(const QString &left, const QString &right);
    int pairIndex() const;
    QString chooseBackup();
    conflictbench::ScanResult scan_;
    QString backupRoot_;
    std::unique_ptr<QTemporaryDir> demo_;
    QTreeWidget *tree_;
    QPlainTextEdit *leftText_, *rightText_, *diff_;
    QLabel *leftInfo_, *rightInfo_, *leftImage_, *rightImage_, *folder_, *folderName_, *status_, *comparisonScope_;
    QTabWidget *tabs_;
    QComboBox *action_;
    QPushButton *review_, *external_, *cleanIdentical_;
    bool busy_ = false;
};
