#pragma execution_character_set("utf-8")
#include "mainwindow.h"

#include <QApplication>
#include <QFont>
#include <QMessageBox>
#include <QSharedMemory>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("RemoteReleaseSync");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("LocalTools");
    app.setQuitOnLastWindowClosed(false);
    const bool startupLaunch = app.arguments().contains("--startup");

    QSharedMemory singleInstanceGuard("LocalTools.RemoteReleaseSync.SingleInstance");
    if (!singleInstanceGuard.create(1)) {
        if (startupLaunch) {
            return 0;
        }
        QMessageBox::information(nullptr, "提示", "程序已经在运行。");
        return 0;
    }

    QFont font;
    font.setPointSize(10);
    app.setFont(font);

    MainWindow window;
    if (!startupLaunch) {
        window.show();
    }

    return app.exec();
}
