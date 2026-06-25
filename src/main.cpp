#include "mainwindow.h"

#include <QApplication>
#include <QFont>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("RemoteReleaseSync");
    app.setOrganizationName("LocalTools");
    app.setQuitOnLastWindowClosed(false);

    QFont font;
    font.setPointSize(10);
    app.setFont(font);

    MainWindow window;
    window.show();

    return app.exec();
}
