#include "workbench.h"
#include <QtWidgets>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QStringDecoder>
#include <atomic>
#include <functional>
using namespace conflictbench;
namespace {
constexpr qint64 inlinePreviewBytes = 1024 * 1024;
const QColor removedLine(255, 226, 226), addedLine(222, 245, 226);
class FitImageLabel final : public QLabel {
    QPixmap image_;
    void fit() { if(!image_.isNull()) QLabel::setPixmap(image_.scaled(contentsRect().size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)); }
public:
    FitImageLabel() { setMinimumSize(1,1); setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Ignored); setAlignment(Qt::AlignCenter); }
    void setImage(const QImage &image) { image_=QPixmap::fromImage(image); fit(); }
protected:
    void resizeEvent(QResizeEvent *event) override { QLabel::resizeEvent(event); fit(); }
};
template<class T, class F> T work(QWidget *owner, const QString &title, F operation) {
    // Qt's worker pool keeps hashing and file operations away from the UI thread.
    // https://doc.qt.io/qt-6.8/qtconcurrentrun.html
    QProgressDialog progress(title, QCoreApplication::translate("Workbench", "Cancel"), 0, 0, owner);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    std::atomic_bool cancelled{false};
    QObject::connect(&progress, &QProgressDialog::canceled, [&] { cancelled = true; });
    Options opts;
    opts.cancelled = [&] { return cancelled.load(); };
    QFutureWatcher<T> watcher;
    QEventLoop loop;
    QObject::connect(&watcher, &QFutureWatcher<T>::finished, &loop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([&] { return operation(opts); }));
    if (!watcher.isFinished()) loop.exec();
    progress.close();
    return watcher.result();
}
QLabel *label(const QString &text = {}) {
    auto *w = new QLabel(text); w->setTextFormat(Qt::PlainText); w->setWordWrap(true);
    w->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard); return w;
}
QPlainTextEdit *editor(const QString &name) {
    auto *w = new QPlainTextEdit; w->setReadOnly(true); w->setObjectName(name);
    w->setAccessibleName(name); w->setLineWrapMode(QPlainTextEdit::NoWrap);
    w->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont)); return w;
}
QString shortName(const QString &name) { return name.size()>28 ? name.left(20)+"…"+name.right(7) : name; }
QString metadata(const Snapshot &s) {
    return QCoreApplication::translate("Workbench", "%1\n%2 bytes · %3 UTC\nSHA-256 %4")
        .arg(shortName(QFileInfo(s.path).fileName())).arg(s.size)
        .arg(s.modifiedUtc.toString("yyyy-MM-dd HH:mm:ss")).arg(QString::fromLatin1(s.sha256.toHex().left(24))+"…");
}
// Syncthing names conflicts <name>.sync-conflict-<YYYYMMDD>-<HHMMSS>-<device>.<ext>.
// The time is when the conflict was found; the device is the first part of its ID.
QString conflictLabel(const Snapshot &conflict, const Snapshot &original) {
    static const QRegularExpression pattern(QStringLiteral(R"(\.sync-conflict-(\d{8})-(\d{6})-([A-Z2-7]{7}))"));
    const QString name = QFileInfo(conflict.path).fileName();
    const auto match = pattern.match(name);
    QString text = shortName(name);
    if(match.hasMatch()) {
        const QDateTime when(QDate::fromString(match.captured(1), "yyyyMMdd"), QTime::fromString(match.captured(2), "HHmmss"));
        if(when.isValid())
            text = QCoreApplication::translate("Workbench", "Conflict · %1 %2 · device %3").arg(QLocale().toString(when.date(), QLocale::ShortFormat), when.toString("HH:mm"), match.captured(3));
    }
    if(conflict.sha256 == original.sha256) text += QCoreApplication::translate("Workbench", " · identical");
    return text;
}
bool textContent(const QByteArray &b, QString *out) {
    if(b.contains('\0')) return false;
    QStringDecoder decoder(QStringDecoder::Utf8);
    *out = decoder(b);
    if(decoder.hasError()) return false;
    for(QChar c : *out) if(c.category() == QChar::Other_Control && c != '\n' && c != '\r' && c != '\t') return false;
    return true;
}
// One entry per output line: ' ' unchanged, '-' only in the original, '+' only in the conflict.
struct DiffLine { char kind; qsizetype left, right; };
bool lineOps(const QStringList &x, const QStringList &y, QVector<DiffLine> *ops) {
    if(x.size()>1200 || y.size()>1200) return false;
    const qsizetype stride=y.size()+1;
    QVector<int> lcs((x.size()+1)*stride,0);
    for(qsizetype i=x.size();i-->0;) for(qsizetype j=y.size();j-->0;)
        lcs[i*stride+j]=x[i]==y[j] ? 1+lcs[(i+1)*stride+j+1] : qMax(lcs[(i+1)*stride+j],lcs[i*stride+j+1]);
    qsizetype i=0,j=0;
    while(i<x.size() || j<y.size()) {
        if(i<x.size() && j<y.size() && x[i]==y[j]) ops->append({' ',i++,j++});
        else if(j<y.size() && (i==x.size() || lcs[i*stride+j+1]>=lcs[(i+1)*stride+j])) ops->append({'+',-1,j++});
        else ops->append({'-',i++,-1});
    }
    return true;
}
QString lineDiff(const QString &a, const QString &b) {
    const auto x=a.split('\n'), y=b.split('\n');
    QVector<DiffLine> ops;
    if(!lineOps(x,y,&ops)) return QCoreApplication::translate("Workbench", "Inline diff is limited to 1,200 lines per version. Use the side-by-side preview or an external diff tool.");
    QStringList lines{"--- original", "+++ conflict"};
    for(const auto &op : ops) lines << (op.kind==' ' ? "  "+x[op.left] : op.kind=='-' ? "- "+x[op.left] : "+ "+y[op.right]);
    return lines.join('\n');
}
void markLines(QPlainTextEdit *edit, const QVector<qsizetype> &lines, const QColor &color) {
    QList<QTextEdit::ExtraSelection> marks;
    for(qsizetype line : lines) {
        QTextEdit::ExtraSelection mark;
        mark.cursor = QTextCursor(edit->document()->findBlockByNumber(int(line)));
        mark.format.setBackground(color);
        mark.format.setProperty(QTextFormat::FullWidthSelection, true);
        marks << mark;
    }
    edit->setExtraSelections(marks);
}
bool writeDemo(const QString &path, const QByteArray &data) {
    QFile f(path); return f.open(QIODevice::WriteOnly | QIODevice::NewOnly) && f.write(data)==data.size();
}
QString planSummary(const Plan &p) {
    const QString original = QFileInfo(p.pair.original.path).fileName();
    switch(p.action) {
    case Action::KeepOriginal: return QCoreApplication::translate("Workbench", "Keep %1. The conflict copy moves to the backup folder.").arg(original);
    case Action::UseConflict: return QCoreApplication::translate("Workbench", "Replace %1 with the conflict copy. The current file is saved in the backup folder.").arg(original);
    case Action::KeepBoth: return QCoreApplication::translate("Workbench", "Keep both. The conflict copy is renamed to %1.").arg(QFileInfo(p.keepBothPath).fileName());
    }
    return {};
}
}
Workbench::Workbench(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(tr("ConflictBench — review before resolving")); resize(1180,820); setMinimumSize(850,600);
    auto *central=new QWidget; auto *layout=new QVBoxLayout(central); layout->setContentsMargins(22,18,22,18); layout->setSpacing(10);
    auto *heading=label("ConflictBench"); QFont title=font(); title.setPointSize(title.pointSize()+11); title.setBold(true); heading->setFont(title); layout->addWidget(heading);
    layout->addWidget(label(tr("Review both versions, preview the plan, then save a verified backup. Identical copies can be cleaned up together.")));
    auto *bar=new QHBoxLayout;
    auto button=[&](const QString &text, const QString &name, auto slot) {auto *b=new QPushButton(text); b->setObjectName(name); b->setAccessibleName(text); connect(b,&QPushButton::clicked,this,slot); bar->addWidget(b); return b;};
    auto *open=button(tr("&Open folder…"),"openFolder",[this]{auto path=QFileDialog::getExistingDirectory(this,tr("Choose a local synced folder"));if(!path.isEmpty())scanFolder(path);});
    open->setShortcut(QKeySequence::Open);
    button(tr("Try &demo"),"demo",[this]{createDemo();});
    button(tr("&History / recovery…"),"history",[this]{historyDialog();});
    button(tr("Scan details…"),"scanDetails",[this]{
        QDialog dialog(this); dialog.setWindowTitle(tr("Scan details")); dialog.resize(720,440);
        auto *layout=new QVBoxLayout(&dialog); auto *details=editor(tr("Scan details"));
        details->setPlainText(scan_.root+"\n\n"+(scan_.warnings.isEmpty()?tr("No skipped-file warnings in the latest scan."):scan_.warnings.join("\n")));
        layout->addWidget(details); auto *close=new QDialogButtonBox(QDialogButtonBox::Close); layout->addWidget(close);
        connect(close,&QDialogButtonBox::rejected,&dialog,&QDialog::reject); dialog.exec();
    });
    bar->addStretch(); button(tr("&Guide"),"guide",[this]{help();}); layout->addLayout(bar);
    folderName_=label(tr("No folder selected")); folderName_->setObjectName("folderName"); QFont bold=folderName_->font(); bold.setBold(true); folderName_->setFont(bold);
    folder_=label(tr("Try the demo using disposable files, or open one local folder.")); folder_->setObjectName("folderPath"); folder_->setWordWrap(false);
    QFont small=folder_->font(); small.setPointSizeF(small.pointSizeF()*0.9); folder_->setFont(small); folder_->setForegroundRole(QPalette::PlaceholderText);
    auto *where=new QHBoxLayout; where->addWidget(folderName_); where->addWidget(folder_,1); layout->addLayout(where);
    auto *split=new QSplitter;
    tree_=new QTreeWidget; tree_->setHeaderLabels({tr("Original / conflict version")}); tree_->setObjectName("conflicts"); tree_->setAccessibleName(tr("Conflict groups and versions")); tree_->setMinimumWidth(240);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection); connect(tree_,&QTreeWidget::itemSelectionChanged,this,[this]{showPair();}); split->addWidget(tree_);
    tabs_=new QTabWidget; tabs_->setAccessibleName(tr("Version previews"));
    auto *versions=new QWidget; auto *columns=new QHBoxLayout(versions);
    auto previewColumn=[&](const QString &title,const QString &name,QLabel **info,QPlainTextEdit **text,QLabel **img) {
        auto *box=new QGroupBox(title); auto *col=new QVBoxLayout(box);
        *info=label(); (*info)->setAccessibleName(name+" metadata"); col->addWidget(*info);
        *text=editor(name+" text preview"); col->addWidget(*text,1);
        *img=new FitImageLabel; (*img)->setAlignment(Qt::AlignCenter); (*img)->setAccessibleName(name+" image preview"); (*img)->hide(); col->addWidget(*img,1); columns->addWidget(box,1);
    };
    previewColumn(tr("Original"),"Original",&leftInfo_,&leftText_,&leftImage_);
    previewColumn(tr("Conflict"),"Conflict",&rightInfo_,&rightText_,&rightImage_);
    tabs_->addTab(versions,tr("&Versions")); diff_=editor("Text difference"); tabs_->addTab(diff_,tr("&Text difference")); split->addWidget(tabs_); split->setStretchFactor(1,1); split->setSizes({300,840}); layout->addWidget(split,1);
    comparisonScope_=label(); comparisonScope_->setObjectName("comparisonScope"); comparisonScope_->setAccessibleName(tr("Comparison scope")); comparisonScope_->hide(); layout->addWidget(comparisonScope_);
    auto *decision=new QHBoxLayout;
    auto *actionLabel=label(tr("&Decision:")); action_=new QComboBox; action_->setObjectName("decision"); action_->setAccessibleName(tr("Resolution decision"));
    action_->addItems({tr("Keep original · archive conflict"),tr("Use conflict · replace original"),tr("Keep both · give conflict a new name")});
    actionLabel->setBuddy(action_); decision->addWidget(actionLabel); decision->addWidget(action_,1);
    external_=new QPushButton(tr("External &diff…")); external_->setObjectName("externalDiff"); connect(external_,&QPushButton::clicked,this,[this]{externalDiff();}); decision->addWidget(external_);
    cleanIdentical_=new QPushButton(tr("Clean up identical copies…")); cleanIdentical_->setObjectName("cleanIdentical"); cleanIdentical_->setToolTip(tr("Archive every conflict copy whose content is exactly the same as its original, in one reviewed step."));
    connect(cleanIdentical_,&QPushButton::clicked,this,[this]{cleanIdentical();}); decision->addWidget(cleanIdentical_);
    review_=new QPushButton(tr("&Review plan…")); review_->setObjectName("reviewPlan"); review_->setDefault(true); connect(review_,&QPushButton::clicked,this,[this]{review();}); decision->addWidget(review_); layout->addLayout(decision);
    status_=label(tr("Scanning is read-only. Pause Syncthing and close editors before any commit or undo.")); status_->setObjectName("status"); layout->addWidget(status_); setCentralWidget(central);
    review_->setEnabled(false); external_->setEnabled(false); cleanIdentical_->setEnabled(false);
}
void Workbench::closeEvent(QCloseEvent *e) {if(busy_) {e->ignore();return;}QMainWindow::closeEvent(e);}
int Workbench::pairIndex() const {
    auto *item=tree_->currentItem(); if(!item || !item->data(0,Qt::UserRole).isValid())return -1;
    const int index=item->data(0,Qt::UserRole).toInt();
    return index>=0 && index<scan_.pairs.size() ? index : -1;
}
void Workbench::scanFolder(const QString &folder) {
    if(busy_)return;
    if(!scan_.root.isEmpty() && QFileInfo(folder).canonicalFilePath()!=scan_.root) backupRoot_.clear();
    busy_=true; centralWidget()->setEnabled(false);
    auto result=work<ScanResult>(this,tr("Scanning local conflict files…"),[&](const Options &o){return scan(folder,o);});
    busy_=false; centralWidget()->setEnabled(true);
    if(!result.error.isEmpty()) {QMessageBox::warning(this,tr("Scan stopped"),result.error);return;}
    QSignalBlocker treeSignals(tree_);
    scan_=result; tree_->clear();
    folderName_->setText(QFileInfo(scan_.root).fileName());
    folder_->setProperty("fullPath",scan_.root); folder_->setToolTip(scan_.root);
    folder_->setText(folder_->fontMetrics().elidedText(scan_.root,Qt::ElideMiddle,520));
    QMap<QString,QTreeWidgetItem*> groups; int identical=0;
    for(int i=0;i<scan_.pairs.size();++i) {
        const auto &p=scan_.pairs[i];
        if(!groups.contains(p.original.path)) {auto *g=new QTreeWidgetItem(tree_,{QDir(scan_.root).relativeFilePath(p.original.path)}); groups.insert(p.original.path,g);g->setExpanded(true);}
        auto *item=new QTreeWidgetItem(groups[p.original.path],{conflictLabel(p.conflict,p.original)});item->setData(0,Qt::UserRole,i);item->setToolTip(0,p.conflict.path);
        if(p.original.sha256==p.conflict.sha256) ++identical;
    }
    cleanIdentical_->setEnabled(identical>0);
    cleanIdentical_->setText(identical>0 ? tr("Clean up %n identical copies…","",identical) : tr("Clean up identical copies…"));
    if(!scan_.pairs.isEmpty())tree_->setCurrentItem(tree_->topLevelItem(0)->child(0));
    treeSignals.unblock();
    showPair();
    status_->setText(tr("%1 conflict version(s) in %2 group(s). %3").arg(scan_.pairs.size()).arg(groups.size()).arg(scan_.warnings.isEmpty()?tr("Choose a version to review."):tr("%1 skipped-file warning(s); open Scan details.").arg(scan_.warnings.size())));
}
void Workbench::createDemo() {
    if(busy_)return;
    demo_=std::make_unique<QTemporaryDir>(QDir::tempPath()+"/conflictbench-demo-XXXXXX");
    if(!demo_->isValid()) {QMessageBox::warning(this,tr("Demo"),tr("Cannot create temporary demo folder."));return;}
    const QString demoPath=QFileInfo(demo_->path()).canonicalFilePath();
    QString root=demoPath+"/Synced demo"; QDir().mkpath(root+"/.stfolder");QDir().mkpath(demoPath+"/Backups");
    bool ok=writeDemo(root+"/여행 계획.txt",QString("Seoul weekend / 서울 여행\nSaturday: palace at 10:00\nLunch: noodles\nBudget: 120\n").toUtf8());
    ok &= writeDemo(root+"/여행 계획.sync-conflict-20261001-091530-ABCDEFG.txt",QString("Seoul weekend / 서울 여행\nSaturday: museum at 11:00\nLunch: noodles\nBudget: 150\nBring an umbrella.\n").toUtf8());
    ok &= writeDemo(root+"/budget.v2.dat",QByteArray::fromHex("0011223344556677"));
    ok &= writeDemo(root+"/budget.v2.sync-conflict-20261001-091530-DESKTOP.dat",QByteArray::fromHex("0011229944556677"));
    QImage original(320,180,QImage::Format_RGB32);original.fill(QColor("#326764")); QPainter a(&original);a.setPen(Qt::white);a.setFont(QFont(QFontDatabase::systemFont(QFontDatabase::GeneralFont).family(),24));a.drawText(original.rect(),Qt::AlignCenter,"Original");a.end();
    QImage conflict(320,180,QImage::Format_RGB32);conflict.fill(QColor("#74512e"));QPainter b(&conflict);b.setPen(Qt::white);b.setFont(QFont(QFontDatabase::systemFont(QFontDatabase::GeneralFont).family(),24));b.drawText(conflict.rect(),Qt::AlignCenter,"Conflict");b.end();
    ok &= original.save(root+"/design.png");ok &= conflict.save(root+"/design.sync-conflict-20261001-091530-ABCDEFG.png");
    if(!ok){QMessageBox::warning(this,tr("Demo"),tr("Could not write all synthetic fixtures."));return;}
    scanFolder(root);
    backupRoot_=demoPath+"/Backups";
    for(int i=0;i<tree_->topLevelItemCount();++i)if(tree_->topLevelItem(i)->text(0).contains("여행"))tree_->setCurrentItem(tree_->topLevelItem(i)->child(0));
    status_->setText(tr("DEMO · Disposable files and backups. They are removed when the app closes. All three decisions and undo can be tried here."));
}
void Workbench::highlightDifferences(const QString &left, const QString &right) {
    const auto x=left.split('\n'), y=right.split('\n');
    QVector<DiffLine> ops;
    if(!lineOps(x,y,&ops)) {markLines(leftText_,{},removedLine);markLines(rightText_,{},addedLine);markLines(diff_,{},addedLine);return;}
    QVector<qsizetype> removed, added, diffRemoved, diffAdded;
    for(qsizetype n=0;n<ops.size();++n) {
        if(ops[n].kind=='-') {removed << ops[n].left; diffRemoved << n+2;}
        if(ops[n].kind=='+') {added << ops[n].right; diffAdded << n+2;}
    }
    markLines(leftText_,removed,removedLine);
    markLines(rightText_,added,addedLine);
    QList<QTextEdit::ExtraSelection> marks;
    for(const auto &[lines,color] : {std::pair{diffRemoved,removedLine},std::pair{diffAdded,addedLine}}) for(qsizetype line : lines) {
        QTextEdit::ExtraSelection mark; mark.cursor=QTextCursor(diff_->document()->findBlockByNumber(int(line)));
        mark.format.setBackground(color); mark.format.setProperty(QTextFormat::FullWidthSelection,true); marks << mark;
    }
    diff_->setExtraSelections(marks);
}
void Workbench::showPair() {
    if(busy_)return;
    int index=pairIndex();review_->setEnabled(index>=0);external_->setEnabled(index>=0);
    leftInfo_->clear();rightInfo_->clear();leftText_->clear();rightText_->clear();diff_->clear();leftImage_->hide();rightImage_->hide();leftText_->show();rightText_->show();comparisonScope_->clear();comparisonScope_->hide();
    leftText_->setExtraSelections({});rightText_->setExtraSelections({});diff_->setExtraSelections({});
    if(index<0)return;
    auto pair=scan_.pairs[index];leftInfo_->setText(metadata(pair.original));rightInfo_->setText(metadata(pair.conflict));
    leftInfo_->setToolTip(pair.original.path+"\nSHA-256 "+pair.original.sha256.toHex());
    rightInfo_->setToolTip(pair.conflict.path+"\nSHA-256 "+pair.conflict.sha256.toHex());
    busy_=true;centralWidget()->setEnabled(false);
    auto previews=work<QPair<Preview,Preview>>(this,tr("Verifying preview hashes…"),[&](const Options&o){return qMakePair(preview(pair.original,inlinePreviewBytes,o),preview(pair.conflict,inlinePreviewBytes,o));});
    busy_=false;centralWidget()->setEnabled(true);
    if(!previews.first.error.isEmpty() || !previews.second.error.isEmpty()) {diff_->setPlainText(previews.first.error+"\n"+previews.second.error+"\n"+tr("Reopen the folder to scan again."));review_->setEnabled(false);external_->setEnabled(false);return;}
    QString left,right;bool leftIsText=textContent(previews.first.bytes,&left),rightIsText=textContent(previews.second.bytes,&right);
    auto render=[&](const Preview &p,bool isText,const QString &text,QPlainTextEdit *edit,QLabel *img) {
        if(isText){edit->setLineWrapMode(QPlainTextEdit::NoWrap);edit->setPlainText((p.truncated?tr("[Partial preview: first 1 MiB only; remaining content is not shown.]")+"\n\n":QString())+text);return;}
        // Decode only a bounded image; malformed/huge inputs fall back to metadata.
        QBuffer buffer;buffer.setData(p.bytes);buffer.open(QIODevice::ReadOnly);QImageReader reader(&buffer);QImageReader::setAllocationLimit(32);
        QSize size=reader.size();
        if(!p.truncated && size.isValid() && qint64(size.width())*size.height()<=16000000) {
            reader.setScaledSize(size.boundedTo(QSize(1200,800)));QImage image=reader.read();
            if(!image.isNull()){static_cast<FitImageLabel*>(img)->setImage(image);img->show();edit->hide();return;}
        }
        edit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        edit->setPlainText(tr("Binary or unsupported text encoding.\n\nUse metadata or an external viewer to inspect it, then choose a decision explicitly.\n\nNo automatic winner is inferred from size or date."));
    };
    render(previews.first,leftIsText,left,leftText_,leftImage_);render(previews.second,rightIsText,right,rightText_,rightImage_);
    const bool partial=previews.first.truncated || previews.second.truncated;
    QString scope;
    if(partial) {
        scope=tr("Partial preview: at least one file exceeds 1 MiB. Inline text difference is unavailable because remaining content is not shown. Review both complete files before deciding.");
        diff_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        diff_->setPlainText(scope+"\n\n"+tr("Matching visible text does not establish identical files. Use External diff for complete snapshots up to 16 MiB per file, or inspect larger files in a trusted viewer."));
    } else if(leftIsText && rightIsText) {
        const bool tooManyLines=left.count('\n')>=1200 || right.count('\n')>=1200;
        scope=tooManyLines ? tr("Complete text previews are available. Inline text difference is unavailable above 1,200 lines per version; review Versions or use External diff.")
                           : tr("Text previews and inline text difference cover both complete files. Red lines are only in the original, green lines only in the conflict.");
        diff_->setLineWrapMode(QPlainTextEdit::NoWrap);
        diff_->setPlainText(lineDiff(left,right));
        if(!tooManyLines) highlightDifferences(left,right);
    } else {
        scope=tr("Text comparison is unavailable for this pair. Review the images or metadata and choose explicitly.");
        diff_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        diff_->setPlainText(tr("A text diff is unavailable for this pair. Review the images or metadata and choose explicitly."));
    }
    scope += pair.original.sha256==pair.conflict.sha256 ? " "+tr("Whole-file SHA-256 hashes are identical.") : " "+tr("Whole-file SHA-256 hashes differ.");
    comparisonScope_->setText(scope);comparisonScope_->show();
}
QString Workbench::chooseBackup() {
    if(!backupRoot_.isEmpty())return backupRoot_;
    backupRoot_=QFileDialog::getExistingDirectory(this,tr("Choose an existing private backup folder outside every synced folder"));return backupRoot_;
}
void Workbench::review() {
    int index=pairIndex();if(index<0 || busy_)return;QString backup=chooseBackup();if(backup.isEmpty())return;
    Plan p=plan(scan_.pairs[index],Action(action_->currentIndex()),backup);
    if(!p.error.isEmpty()){QMessageBox::warning(this,tr("Plan unavailable"),p.error);backupRoot_.clear();return;}
    QDialog dialog(this);dialog.setWindowTitle(tr("Review transaction plan"));dialog.setObjectName("planDialog");auto *layout=new QVBoxLayout(&dialog);
    auto *summary=label(planSummary(p));QFont strong=summary->font();strong.setBold(true);summary->setFont(strong);summary->setObjectName("planSummary");layout->addWidget(summary);
    auto *table=new QFormLayout;
    auto row=[&](const QString &name,const QString &value,const QString &tip){auto *v=label(value);v->setToolTip(tip);table->addRow(name,v);};
    row(tr("Original"),QDir(p.pair.root).relativeFilePath(p.pair.original.path),p.pair.original.path);
    row(tr("Conflict copy"),conflictLabel(p.pair.conflict,p.pair.original),p.pair.conflict.path);
    if(!p.keepBothPath.isEmpty()) row(tr("New name"),QFileInfo(p.keepBothPath).fileName(),p.keepBothPath);
    row(tr("Backup folder"),QFileInfo(p.backupRoot).fileName(),p.backupRoot);
    layout->addLayout(table);
    auto *scope=label(comparisonScope_->text()+"\n"+tr("This decision applies to the whole files, including any content not displayed."));scope->setObjectName("planComparisonScope");scope->setAccessibleName(tr("Plan comparison scope"));layout->addWidget(scope);
    auto *details=new QToolButton;details->setText(tr("Show full paths and SHA-256"));details->setCheckable(true);details->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);details->setArrowType(Qt::RightArrow);layout->addWidget(details);
    auto *text=editor("Transaction plan");text->setPlainText(p.description+"\n\nOriginal: "+p.pair.original.path+"\nConflict: "+p.pair.conflict.path+"\nBackup folder: "+p.backupRoot+(p.keepBothPath.isEmpty()?"":"\nKeep-both destination: "+p.keepBothPath)+"\n\nOriginal SHA-256: "+p.pair.original.sha256.toHex()+"\nConflict SHA-256: "+p.pair.conflict.sha256.toHex()+"\n\n"+tr("Only this selected conflict is resolved. Other versions remain.\nThis operation has recorded stages, not an atomic multi-file guarantee. On interruption, use History / recovery."));
    text->setAccessibleName(tr("Transaction plan"));text->hide();layout->addWidget(text,1);
    connect(details,&QToolButton::toggled,&dialog,[&dialog,text,details](bool on){
        text->setVisible(on);details->setArrowType(on?Qt::DownArrow:Qt::RightArrow);
        QTimer::singleShot(0,&dialog,[&dialog,on]{dialog.resize(dialog.width(),on?qMax(dialog.height(),560):dialog.sizeHint().height());});
    });
    auto *paused=new QCheckBox(tr("I paused Syncthing for this folder and closed all other file writers."));paused->setObjectName("pauseAcknowledgment");layout->addWidget(paused);
    auto *privateBackup=new QCheckBox(tr("The backup folder is private, local, and outside ALL synced folders."));privateBackup->setObjectName("backupAcknowledgment");layout->addWidget(privateBackup);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Cancel);auto *commit=buttons->addButton(tr("Save backups and commit"),QDialogButtonBox::AcceptRole);commit->setObjectName("commit");commit->setEnabled(false);layout->addWidget(buttons);
    auto enable=[=]{commit->setEnabled(paused->isChecked()&&privateBackup->isChecked());};connect(paused,&QCheckBox::toggled,enable);connect(privateBackup,&QCheckBox::toggled,enable);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    dialog.resize(680,dialog.sizeHint().height());
    if(dialog.exec()!=QDialog::Accepted)return;
    busy_=true;centralWidget()->setEnabled(false);Result result=work<Result>(this,tr("Saving backups and applying the reviewed decision…"),[&](const Options&o){return execute(p,true,o);});busy_=false;centralWidget()->setEnabled(true);
    QString message=tr("State: %1").arg(result.state)+"\n"+result.error+(result.receiptPath.isEmpty()?"":"\n"+tr("Receipt: %1").arg(result.receiptPath));
    if(result.ok)QMessageBox::information(this,tr("Resolution saved"),message+"\n"+tr("Local undo is available from History / recovery. Resume Syncthing only after verifying the result."));else QMessageBox::warning(this,tr("Transaction stopped"),message+"\n"+tr("Review the receipt in History / recovery before resuming sync."));
    scanFolder(scan_.root);
}
void Workbench::cleanIdentical() {
    if(busy_)return;
    QVector<ConflictPair> pairs;
    for(const auto &pair : scan_.pairs) if(pair.original.sha256==pair.conflict.sha256) pairs << pair;
    if(pairs.isEmpty())return;
    QString backup=chooseBackup();if(backup.isEmpty())return;
    QVector<Plan> plans;
    for(const auto &pair : pairs) {
        Plan p=plan(pair,Action::KeepOriginal,backup);
        if(!p.error.isEmpty()){QMessageBox::warning(this,tr("Plan unavailable"),p.error);backupRoot_.clear();return;}
        plans << p;
    }
    QDialog dialog(this);dialog.setWindowTitle(tr("Clean up identical copies"));dialog.setObjectName("batchDialog");dialog.resize(720,480);auto *layout=new QVBoxLayout(&dialog);
    auto *summary=label(tr("These %n conflict copies have exactly the same content (SHA-256) as their originals. Each copy moves to the backup folder with its own receipt; the originals stay where they are.","",int(plans.size())));
    QFont strong=summary->font();strong.setBold(true);summary->setFont(strong);layout->addWidget(summary);
    auto *list=new QTreeWidget;list->setObjectName("batchList");list->setHeaderLabels({tr("Original"),tr("Conflict copy")});list->setRootIsDecorated(false);
    for(const auto &p : plans) {auto *item=new QTreeWidgetItem(list,{QDir(p.pair.root).relativeFilePath(p.pair.original.path),conflictLabel(p.pair.conflict,p.pair.original)});item->setToolTip(1,p.pair.conflict.path);}
    list->header()->setSectionResizeMode(0,QHeaderView::ResizeToContents);layout->addWidget(list,1);
    layout->addWidget(label(tr("Backup folder: %1").arg(backup)));
    auto *paused=new QCheckBox(tr("I paused Syncthing for this folder and closed all other file writers."));paused->setObjectName("pauseAcknowledgment");layout->addWidget(paused);
    auto *privateBackup=new QCheckBox(tr("The backup folder is private, local, and outside ALL synced folders."));privateBackup->setObjectName("backupAcknowledgment");layout->addWidget(privateBackup);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Cancel);auto *commit=buttons->addButton(tr("Move %n copies to the backup","",int(plans.size())),QDialogButtonBox::AcceptRole);commit->setObjectName("commit");commit->setEnabled(false);layout->addWidget(buttons);
    auto enable=[=]{commit->setEnabled(paused->isChecked()&&privateBackup->isChecked());};connect(paused,&QCheckBox::toggled,enable);connect(privateBackup,&QCheckBox::toggled,enable);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if(dialog.exec()!=QDialog::Accepted)return;
    busy_=true;centralWidget()->setEnabled(false);
    // Each copy is its own recorded transaction, verified again before it runs.
    // The batch stops at the first one that does not commit.
    auto results=work<QVector<Result>>(this,tr("Archiving identical conflict copies…"),[&](const Options&o){
        QVector<Result> done;
        for(const auto &p : plans) {done << execute(p,true,o); if(!done.last().ok || (o.cancelled && o.cancelled())) break;}
        return done;
    });
    busy_=false;centralWidget()->setEnabled(true);
    const auto committed=std::count_if(results.begin(),results.end(),[](const Result &r){return r.ok;});
    if(committed==plans.size())QMessageBox::information(this,tr("Identical copies archived"),tr("%n conflict copies moved to the backup. Local undo is available from History / recovery.","",int(committed)));
    else QMessageBox::warning(this,tr("Clean-up stopped"),tr("%1 of %2 copies were archived.").arg(committed).arg(plans.size())+(results.isEmpty()?QString():"\n"+tr("State: %1").arg(results.last().state)+"\n"+results.last().error)+"\n"+tr("Review the receipts in History / recovery before resuming sync."));
    scanFolder(scan_.root);
}
void Workbench::historyDialog() {
    if(busy_)return;
    QString location=QFileDialog::getExistingDirectory(this,tr("Open the backup folder used for earlier transactions"),backupRoot_);if(location.isEmpty())return;backupRoot_=location;
    QDialog dialog(this);dialog.setWindowTitle(tr("History / recovery"));dialog.resize(850,520);auto *layout=new QVBoxLayout(&dialog);
    layout->addWidget(label(tr("Receipts and backups stay in the chosen folder. Recovery restores only recognized local states; later or remote-device changes are never assumed safe to undo.")));
    auto receipts=history(location);auto *list=new QTreeWidget;list->setHeaderLabels({tr("Time (UTC)"),tr("Decision"),tr("State"),tr("Original")});list->setAccessibleName(tr("Transaction history"));
    for(int i=0;i<receipts.size();++i){const auto&r=receipts[i];auto *item=new QTreeWidgetItem(list,{r.createdUtc,r.action,r.state,QFileInfo(r.originalPath).fileName()});item->setData(0,Qt::UserRole,i);item->setToolTip(0,r.path+"\n"+r.error);}layout->addWidget(list,1);
    auto *paused=new QCheckBox(tr("I paused Syncthing and closed all writers for this receipt's folder."));layout->addWidget(paused);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Close);auto *undoButton=buttons->addButton(tr("Undo / recover selected"),QDialogButtonBox::ActionRole);undoButton->setEnabled(false);layout->addWidget(buttons);
    auto enable=[=]{undoButton->setEnabled(paused->isChecked()&&list->currentItem());};connect(paused,&QCheckBox::toggled,enable);connect(list,&QTreeWidget::itemSelectionChanged,enable);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(undoButton,&QPushButton::clicked,&dialog,[&]{
        const Receipt r=receipts[list->currentItem()->data(0,Qt::UserRole).toInt()];
        if(QMessageBox::question(&dialog,tr("Confirm local rollback"),tr("Restore the two saved versions for:\n%1\n\nThe current files and backups will be hash-checked first. Changed files cause refusal. Backups are retained.").arg(r.originalPath))!=QMessageBox::Yes)return;
        busy_=true;dialog.setEnabled(false);auto result=work<Result>(this,tr("Verifying and restoring the saved versions…"),[&](const Options&o){return undo(r.path,true,o);});dialog.setEnabled(true);busy_=false;
        QMessageBox::information(&dialog,result.ok?tr("Local rollback complete"):tr("Recovery stopped"),tr("State: %1").arg(result.state)+"\n"+result.error);if(result.ok)dialog.accept();
    });
    dialog.exec();if(!scan_.root.isEmpty())scanFolder(scan_.root);
}
void Workbench::externalDiff() {
    int index=pairIndex();if(index<0||busy_)return;
    QString program=QFileDialog::getOpenFileName(this,tr("Choose a trusted diff executable (not a shell command)"));if(program.isEmpty())return;
    QDialog dialog(this);dialog.setWindowTitle(tr("External diff arguments"));auto *layout=new QVBoxLayout(&dialog);
    layout->addWidget(label(tr("One literal argument per line. Use {original} and {conflict} for read-only snapshot paths. No shell is used. The executable runs with your account's permissions; select a trusted tool.")));
    auto *args=new QPlainTextEdit("{original}\n{conflict}");args->setAccessibleName(tr("Literal arguments, one per line"));layout->addWidget(args);auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);layout->addWidget(buttons);connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);if(dialog.exec()!=QDialog::Accepted)return;
    const auto pair=scan_.pairs[index];busy_=true;centralWidget()->setEnabled(false);
    const auto previews=work<QPair<Preview,Preview>>(this,tr("Preparing verified external diff snapshots…"),[&](const Options&o){return qMakePair(preview(pair.original,16LL*1024*1024,o),preview(pair.conflict,16LL*1024*1024,o));});
    busy_=false;centralWidget()->setEnabled(true);
    if(!previews.first.error.isEmpty()||!previews.second.error.isEmpty()){QMessageBox::warning(this,tr("External diff"),previews.first.error+"\n"+previews.second.error);return;}
    if(previews.first.truncated||previews.second.truncated){QMessageBox::warning(this,tr("External diff"),tr("External snapshots are limited to 16 MiB per file. Use metadata and a separately opened trusted viewer for larger files."));return;}
    auto temp=std::make_shared<QTemporaryDir>(QDir::tempPath()+"/conflictbench-diff-XXXXXX");
    const QString left=temp->path()+"/original."+QFileInfo(pair.original.path).suffix(),right=temp->path()+"/conflict."+QFileInfo(pair.conflict.path).suffix();
    if(!temp->isValid()||!writeDemo(left,previews.first.bytes)||!writeDemo(right,previews.second.bytes)){QMessageBox::warning(this,tr("External diff"),tr("Could not create temporary snapshot files."));return;}
    QFile::setPermissions(left,QFileDevice::ReadOwner);QFile::setPermissions(right,QFileDevice::ReadOwner);
    QStringList argv=args->toPlainText().split('\n',Qt::SkipEmptyParts);bool hasLeft=false,hasRight=false;
    for(QString &arg:argv){hasLeft |= arg.contains("{original}");hasRight |= arg.contains("{conflict}");arg.replace("{original}",left).replace("{conflict}",right);}
    if(!hasLeft||!hasRight){QMessageBox::warning(this,tr("External diff"),tr("Include both {original} and {conflict} arguments."));return;}
    auto *process=new QProcess(this);
    // Structured executable + argv; never invoke a shell or interpolate command text.
    // https://doc.qt.io/qt-6.8/qprocess.html#start
    connect(process,&QProcess::errorOccurred,this,[this,process,temp](QProcess::ProcessError){QMessageBox::warning(this,tr("External tool failed"),process->errorString());process->deleteLater();});
    connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this,process,temp](int code,QProcess::ExitStatus exit){if(exit==QProcess::CrashExit||code>1)QMessageBox::warning(this,tr("External tool failed"),tr("Tool exited with code %1. Files in the synced folder were not passed to it.").arg(code));process->deleteLater();});
    process->setStandardOutputFile(QProcess::nullDevice());
    process->setStandardErrorFile(QProcess::nullDevice());
    process->start(program,argv);
}
void Workbench::help() {
    QMessageBox::information(this,tr("ConflictBench guide"),tr("ConflictBench %1\n\n1. Open exactly the local folder you want to inspect. Scanning is read-only.\n2. Select a conflict version. Changed lines are highlighted; images and metadata are shown side by side.\n3. Choose Keep original, Use conflict, or Keep both. Copies identical to their original can be archived together with Clean up identical copies.\n4. Choose a private backup folder outside every synced folder.\n5. Pause Syncthing, close editors, review the plan, and commit.\n6. Verify the result before resuming sync. History offers limited local undo.\n\nNo account, API key, network calls, daemon, or automatic winner.\n\nLimits: local regular files up to 256 MiB; 100,000 entries / 1,000 conflicts per scan. Missing originals, symlinks, and unsafe names are skipped. Case conflicts, ACL/xattr preservation, cloud placeholders, network volumes, and hostile concurrent writers are not supported. Pause is required: hash checks do not remove every race.\n\nGPL-3.0-only · Qt is dynamically linked. Unsigned / not notarized.").arg(CB_VERSION));
}
bool Workbench::smokeTest(QString *error) {
    createDemo();
    if(scan_.pairs.size()!=3 || pairIndex()<0 || leftText_->toPlainText().isEmpty()) {*error="Demo scan or native preview failed";return false;}
    auto p=plan(scan_.pairs[pairIndex()],Action::KeepBoth,backupRoot_);
    if(!p.error.isEmpty()){*error=p.error;return false;}
    auto applied=execute(p,true);if(!applied.ok){*error=applied.error;return false;}
    auto reverted=undo(applied.receiptPath,true);if(!reverted.ok){*error=reverted.error;return false;}
    if(scan(scan_.root).pairs.size()!=3){*error="Undo did not restore demo conflict count";return false;}
    return true;
}
