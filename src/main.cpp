#pragma execution_character_set("utf-8")
#include "mainwindow.h"

#include <QApplication>
#include <QFont>
#include <QIcon>
#include <QMessageBox>
#include <QSharedMemory>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("RemoteReleaseSync");
    app.setApplicationVersion("1.0.2");
    app.setOrganizationName("LocalTools");
    app.setQuitOnLastWindowClosed(false);

    QIcon appIcon;
    for (const int size : {16, 24, 32, 48, 64, 128, 256}) {
        appIcon.addFile(QString(":/icons/app_%1.png").arg(size), QSize(size, size));
    }
    app.setWindowIcon(appIcon);

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
