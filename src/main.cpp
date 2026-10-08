#include "workbench.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>
#include <QTextStream>
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("ConflictBench");
    QCoreApplication::setOrganizationName("ConflictBench");
    QCoreApplication::setApplicationVersion(CB_VERSION);
    QCommandLineParser parser;
    parser.setApplicationDescription("Review Syncthing file conflicts locally, with verified backups and recoverable receipts.");
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({"demo", "Start with temporary synthetic text, image and binary conflicts."});
    parser.addOption({"smoke-test", "Exercise the desktop and a synthetic commit/undo; exit with a status code."});
    parser.addOption({"screenshot", "Save a demo screenshot to this PNG file, then exit.", "path"});
    parser.process(app);
    Workbench window;
    window.show();
    QTimer::singleShot(0, &window, [&] {
        if(parser.isSet("smoke-test")) {
            QString error;
            bool ok = window.smokeTest(&error);
            QTextStream(ok ? stdout : stderr) << (ok ? "PASS desktop synthetic scan/preview/commit/undo\n" : error + "\n");
            app.exit(ok ? 0 : 1);
        } else if(parser.isSet("demo") || parser.isSet("screenshot")) {
            window.createDemo();
            if(parser.isSet("screenshot")) QTimer::singleShot(400, &window, [&] {
                app.exit(window.grab().save(parser.value("screenshot")) ? 0 : 1);
            });
        }
    });
    return app.exec();
}
